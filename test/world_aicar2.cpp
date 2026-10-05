// world_aicar2.cpp -- the world harness for the AI driver's second half (hook/phys_aicar2.cpp): the originals
// and the rewrites, run on the same random AI cars, compared byte for byte.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_aicar2.cpp
//        /Fo<dir>\ /Fe<dir>\world_aicar2.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_aicar2.exe [iterations] [seed] [only this kind, -1 = all] [wild percentage, default 10]
//   VP_VRMOD=1: against vrmod's race.exe (test/vrmod_image.h), with headon.py's ret in headon_panic in half the worlds
//
// out\race_v10.exe is loaded at 0x400000 the way test/fuzz.cpp does it. A real AICar (vtable 0x4dbc28) is built
// in this process's memory with random state, around a small real racing line: a loop of ILSegs (6..25 nodes on
// a circle through the car, `next` pointers, tangents, corridor widths, the braking distances and laterals) with
// the car's SegmentInfo notes on it, a ConstIdealLine and a CenterLine object, the driver's skill record, the
// proxer levels, a Proxer table of up to 5 cars (records with positions and velocities, the sorted ProxerDelta
// list), the deity (a RaceDeity-shaped object that is also RaceDeity::global: per-car lap counts and the
// CenterLine pointer) and the AI track-info table. Everything the functions call runs as the original code in
// the image -- the first half of aicar.obj (_MSG, GetRelSegment, the AISet* controls, set_*_control,
// init_rtinfo, check_for_too_fast / stranded / damage, teleport_to_track, update_line_info /
// update_segment_info, get_ilpos, count_wheels and the wheel predicates), this group's own functions (each
// rewrite is compared with every callee original), the Car setters, ILSeg::QuickTan, ProxerDelta::GetMyDL/DV,
// __CIfmod -- except what reaches outside, patched with a jump to a logging stub:
//   Random (0x41b6e0)                            scripted (script mod the range)
//   PhysicsGetTime (0x42bc80)                    scripted
//   AIGetDriverNameByCar (0x420d40), AIGetTrackNumber (0x420df0), _IsAI (0x41d370)     scripted, logged
//   IdealLine::get_actual_bead_position (0x421c10)  the bead: a scripted ring node (or NULL) and t
//   IdealLine::get_nearest_bead (0x4216f0)       a scripted node (or NULL), t and distance
//   IdealLine::update_car_info (0x4214c0), reset_bead_position (0x421520)             logged
//   CenterLine::get_car_dlong_meters (0x4225e0), convert_meters_to_ilpos (0x422710)   scripted, logged
//   AICar::get_real_target (0x4300f0)            logged (the first half's target picker)
//   Car::Update (0x437680), Car::SetGearAutoAI (0x439760)                             logged
//   the deity's StopRace / GetNumLaps / TeleportToLine / GetCurrentLap                logged, scripted
//   the profiler's function pointers (0x4e642c..0x4e6438)                           logged
// Each world picks a function; the original runs, the state and the logs are kept, the starting state is
// restored, the rewrite runs, and every object, the outputs, the globals, the return value and the logs are
// compared. The bytes the original changed must also lie inside the rewrite's footprint (unless replay_only).
// A share of the worlds ("wild") has floats replaced by extreme values. A fault in both is counted, not
// compared (the NULL-bead crash is one: faithful).
//
// The fixes (port.h: VP_FIX): the plain build defines VP_FAITHFUL and checks the rewrites against the originals as
// above. Built with /DFIX_TESTS it compiles the fixed rewrites, with a fake driver lounge for begin_race's lookup:
// every world must still equal the original, except init_rt_lat on a lost (NULL) bead, where the original faults
// and the fixed one must run (and every byte it changes must be in its footprint); then the fixes on their own:
// init_rt_lat on a NULL bead that reset_bead_position finds (the same result as the original's on that bead) or
// doesn't (the racing-line offset left as it was), and begin_race with driver names of 32 characters and more
// (and a NULL one): no fault, AIGetDriverNameByCar's strcpy not reached, the car as with a short name.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FIX_TESTS
#define VP_FAITHFUL                                 // the original's behaviour, bit for bit (the fixes: /DFIX_TESTS)
#endif
#include "../hook/port.h"
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

// VP_VRMOD=1: vrmod's race.exe (test/vrmod_image.h), and in half the worlds vrmod's opt-in headon.py on top of it --
// AICar::headon_panic's first byte a ret (docs/FIXES.md, "vrmod's patches"). The rewrite asks port_vrmod_has, which in
// the DLL is the stock check's verdict, taken before the hook; here it is whether the world put the ret there.
static bool g_vrmod, g_headon;
bool port_vrmod_has(uint32_t v10) { return v10 == 0x004311d0 && g_headon; }
void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) {
    uint32_t vt = *(uint32_t*)obj;
    if (vt == 0x004dbc28) add(obj, 4224, what);
    else printf("footprint: %s at %p has an unknown class (vtable %08x)\n", what, obj, vt);
}

#include "../hook/phys_aicar2.cpp"
#include "vrmod_image.h"

// ---- random numbers --------------------------------------------------------------------------------------
static uint32_t g_rng = 0x9e3779b9u;
static int g_wild_pct = 10;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float range(float a, float b) { return a + (b - a) * uni(); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float wildf() {
    uint32_t r = rnd(), k = r % 13;
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
    case 9: return bitsf((r & 0x100 ? 0xff800001u : 0x7f800001u) | (rnd() & 0x3ffffe));   // signalling NaN
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

// ---- the world ----------------------------------------------------------------------------------------------
enum { CAR_BUF = 4224, NSEG_MAX = 32, NPINFO = 8, NDELTA = 8, NRECORDS = 512, SKILL_BYTES = 160, DEITY_BYTES = 2012,
       TRACK_TABLE = 16 * 20 };
static uint8_t* g_carbuf;
static AICar* car() { return (AICar*)g_carbuf; }
static uint8_t g_skill[SKILL_BYTES];
static ILSeg g_ring[NSEG_MAX];
static int g_nseg;
static SegmentInfo g_seginfo[NSEG_MAX];
static ProxerInfo g_pinfo[NPINFO];
static uint8_t g_line[60];
static uint8_t g_cline[112];
static uint8_t g_deity_obj[DEITY_BYTES];
static Proxer g_prox;
static uint8_t g_records[NRECORDS];
static ProxerDelta g_delta[NDELTA];
static float g_lap_clock;
static uint8_t* const g_track_table = (uint8_t*)0x004ec6c0;
static int32_t* const g_game_state_p = (int32_t*)0x004e4e3c;
static uint8_t* const g_learn_p = (uint8_t*)0x004eb648;
static uint8_t* const g_replay_play_p = (uint8_t*)0x005218e8;
static int32_t* const g_tick_p = (int32_t*)0x0052161c;

// the arguments of the world's call
struct Args {
    float brakes[4];
    float throttle, brake, steer;
    ILinePos pos;
    float dist;
    Point2D p;                                   // in_path's point, Right's vector
    Point2D hout, hin[4];
    float ht;
    uint32_t f[5];
    uint8_t b;
    float ra, rb;
    int delta;
    ProxerInfo pi;
};
static Args g_args;

// ---- stubs: scripts and the log ------------------------------------------------------------------------------
struct Script {
    int random[8];
    float time;
    float dlong;
    ILSeg* m2i_seg; float m2i_t;
    ILSeg* bead_seg; float bead_t;
    ILSeg* near_seg; float near_t; float near_dist;
    uint8_t isai[16];
    int track;
    int num_laps, cur_lap;
    uint8_t teleport_ok;
};
struct Log { int n; uint32_t e[256]; };
static Script g_script;
static int g_random_i;
static Log g_log;
static void log_put(uint32_t v) { if (g_log.n < 256) g_log.e[g_log.n] = v; g_log.n++; }

static int __cdecl stub_random(int r) {
    log_put(0x4a4d0000u); log_put((uint32_t)r);
    int v = g_script.random[g_random_i++ & 7];
    return r > 0 ? v % r : v;
}
static float __cdecl stub_time() { log_put(0x71000000u); return g_script.time; }
#ifdef FIX_TESTS
// the driver lounge begin_race's fixed lookup goes through (AIDriverLounge::Get: a driver record, its name at +0x210)
static const char* g_driver_name = "Doug";
static uint8_t g_fake_driver[0x220];
static const uint8_t* __fastcall stub_lounge_get(void*, int, int, int) {
    *(const char**)(g_fake_driver + 0x210) = g_driver_name;
    return g_fake_driver;
}
static void* g_fake_lounge_vt[4] = {0, (void*)&stub_lounge_get, 0, 0};
static struct { void** vt; } g_fake_lounge = {g_fake_lounge_vt};
static int g_reset_bead_to = -1;                 // reset_bead_position finds the bead at this ring node (-1: doesn't)
static void __cdecl stub_driver_name(char* buf, int car) { log_put(0xd5a30000u); log_put((uint32_t)car); strcpy(buf, g_driver_name); }
#else
static void __cdecl stub_driver_name(char* buf, int car) { log_put(0xd5a30000u); log_put((uint32_t)car); strcpy(buf, "Doug"); }
#endif
static int __cdecl stub_track_number() { log_put(0x7ac40000u); return g_script.track; }
static uint8_t __cdecl stub_isai(int car) { log_put(0x15a10000u); log_put((uint32_t)car); return g_script.isai[car & 15]; }
static ILinePos* __fastcall stub_bead(IdealLine* line, int, ILinePos* ret) {
    log_put(0xbead0000u); log_put((uint32_t)line);
    ret->seg = g_script.bead_seg; ret->t = g_script.bead_t;
    return ret;
}
static float __fastcall stub_nearest(IdealLine* line, int, const Point2D* p, ILinePos* pos, uint32_t maxd) {
    log_put(0x4ea40000u); log_put((uint32_t)line); log_put(fbits(p->x)); log_put(fbits(p->z));
    log_put((uint32_t)pos->seg); log_put(fbits(pos->t)); log_put(maxd);
    pos->seg = g_script.near_seg; pos->t = g_script.near_t;
    return g_script.near_dist;
}
static void __fastcall stub_update_car_info(IdealLine* line, int, const Point2D* p, const Point2D* v) {
    log_put(0xca410000u); log_put((uint32_t)line); log_put(fbits(p->x)); log_put(fbits(p->z)); log_put(fbits(v->x)); log_put(fbits(v->z));
}
static void __fastcall stub_reset_bead(IdealLine* line, int) {
    log_put(0x4e5e0000u); log_put((uint32_t)line);
#ifdef FIX_TESTS
    if (g_reset_bead_to >= 0) g_script.bead_seg = &g_ring[g_reset_bead_to];
#endif
}
static float __fastcall stub_dlong(CenterLine* cl, int) { log_put(0xd1000000u); log_put((uint32_t)cl); return g_script.dlong; }
static ILinePos* __fastcall stub_m2i(CenterLine* cl, int, ILinePos* ret, uint32_t m) {
    log_put(0x3210000u); log_put((uint32_t)cl); log_put(m);
    ret->seg = g_script.m2i_seg; ret->t = g_script.m2i_t;
    return ret;
}
static void __fastcall stub_real_target(AICar* self, int) { log_put(0x7a400000u); log_put((uint32_t)self); }
static void __fastcall stub_car_update(AICar* self, int) { log_put(0xca400000u); log_put((uint32_t)self); }
static void __fastcall stub_gear_auto_ai(AICar* self, int) { log_put(0x6ea40000u); log_put((uint32_t)self); }
static void __fastcall stub_stop_race(void* d, int) { log_put(0xde170008u); log_put((uint32_t)d); }
static int __fastcall stub_num_laps(void* d, int) { log_put(0xde170020u); log_put((uint32_t)d); return g_script.num_laps; }
static uint8_t __fastcall stub_teleport(void* d, int, int idx, void* c) {
    log_put(0xde17003cu); log_put((uint32_t)d); log_put((uint32_t)idx); log_put((uint32_t)c);
    return g_script.teleport_ok;
}
static int __fastcall stub_cur_lap(void* d, int, int idx) { log_put(0xde170048u); log_put((uint32_t)d); log_put((uint32_t)idx); return g_script.cur_lap; }
static void __cdecl stub_ov_begin() { log_put(0x0f000001u); }
static void __cdecl stub_ov_end() { log_put(0x0f000002u); }
static int __cdecl stub_prof_start(const char* s) { log_put(0x0f000003u); log_put((uint32_t)s); return 77; }
static void __cdecl stub_prof_stop(int id) { log_put(0x0f000004u); log_put((uint32_t)id); }

// ---- building a world --------------------------------------------------------------------------------------
static void rotation(float* m) {
    double yaw = range(-3.1416f, 3.1416f);
    double lim = chance(85) ? 0.3 : 3.1416;
    double pitch = range((float)-lim, (float)lim), roll = range((float)-lim, (float)lim);
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch), cr = cos(roll), sr = sin(roll);
    double R[9] = {cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr,
                   -cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr,
                   sy * cp, -sp, cy * cp};
    for (int i = 0; i < 9; i++) m[i] = (float)R[i];
}
static const uint32_t k_msg_slots[] = {0x4ed408, 0x4ed40c, 0x4ed410, 0x4ed414, 0x4ed418, 0x4ed41c, 0x4ed420, 0x4ed424,
    0x4ed428, 0x4ed42c, 0x4ed430, 0x4ed434, 0x4ed438, 0x4ed43c, 0x4ed440, 0x4ed444, 0x4ed448, 0x4ed44c, 0x4ed450,
    0x4ed454, 0x4ed458, 0x4ed45c, 0x4ed460, 0x4ed464};
enum { NMSG = sizeof k_msg_slots / sizeof *k_msg_slots };

static void randomize_world() {
    AICar* c = car();
    uint8_t* b = g_carbuf;
    memset(b, 0, CAR_BUF);
    c->vtable = (void**)0x004dbc28;
    float* rot = (float*)(b + 0x38);
    rotation(rot);
    P3* pos = (P3*)(b + 0x5c);
    pos->x = range(-1500.0f, 1500.0f); pos->y = range(-20.0f, 200.0f); pos->z = range(-1500.0f, 1500.0f);
    int sk = rnd() % 5;
    const float spd = sk == 0 ? range(0.0f, 0.5f) : sk == 1 ? range(0.4f, 9.0f) : sk == 2 ? range(8.8f, 32.0f) : range(30.0f, 80.0f);
    // the racing line: a circle through the car (node 0 at the car); mostly heading along it
    const float R = range(150.0f, 600.0f), a0 = range(-3.1416f, 3.1416f);
    const float hdg = chance(80) ? -a0 + range(-0.2f, 0.2f) : range(-3.1416f, 3.1416f);
    c->velocity.x = sinf(hdg) * spd + range(-0.5f, 0.5f); c->velocity.y = range(-1.0f, 1.0f); c->velocity.z = cosf(hdg) * spd + range(-0.5f, 0.5f);
    if (chance(3)) { c->velocity.x = 0.0f; c->velocity.z = 0.0f; }
    c->angular_velocity.x = range(-1.0f, 1.0f); c->angular_velocity.y = chance(15) ? 0.0f : range(-2.0f, 2.0f); c->angular_velocity.z = range(-1.0f, 1.0f);
    c->yaw_control = rnd() % 3;
    static const int states[] = {0, 1, 2, 2, 2, 2, 2, 3};
    c->race_state = states[rnd() % 8];
    c->steering = chance(20) ? 0.0f : range(-1.0f, 1.0f);
    c->opacity = chance(70) ? 1.0f : chance(30) ? bitsf(0x3f7d70a4 - 1 + rnd() % 3) : uni();
    for (int i = 0; i < 4; i++) {
        uint8_t* w = (uint8_t*)&c->wheels[i];
        static const int surfaces[] = {0, 0, 0, 0, 0, 0, 14, 14, 1, 5, 10, 15};
        c->wheels[i].surface = chance(70) ? 0 : surfaces[rnd() % 12];
        w[0x168] = (uint8_t)chance(80);                // on_ground
        w[0x20] = (uint8_t)chance(5);                  // broken
        *(int32_t*)(w + 0x15c) = rnd() % 3;            // traction control
        *(float*)(w + 0xdc) = range(-0.5f, 0.5f);      // steer_input
    }
    if (chance(15)) for (int i = 0; i < 4; i++) c->wheels[i].surface = chance(80) ? 14 : 0;   // mostly off / in water
    c->engine_throttle = chance(40) ? 1.0f : uni();
    c->front_axle_z = range(1.0f, 1.6f); c->rear_axle_z = -range(1.0f, 1.6f);
    c->max_steer_angle = range(0.3f, 0.7f);
    *(float*)(b + 0xde4) = uni();                      // braking
    *(float*)(b + 0xde0) = uni();                      // throttle
    *(float*)(b + 0xea4) = range(-2.0f, 2.0f);         // lat_g
    c->lowering = (uint8_t)chance(15);
    c->steer_feedback = range(-1.5f, 1.5f);
    // ---- the driver
    c->drive_fn = (void*)(uintptr_t)(chance(85) ? 0x00431ce0 : 0x00431960);
    c->interact_fn = (void*)(uintptr_t)(chance(85) ? 0x004312a0 : 0x00431950);
    c->ahead_valid = (uint8_t)chance(50);
    c->ahead_dist = chance(20) ? range(4.0f, 6.0f) : range(0.0f, 20.0f);
    c->ahead_rel_speed = chance(10) ? range(-2e-4f, 2e-4f) : range(-15.0f, 5.0f);
    c->side_lat = range(-5.0f, 5.0f); c->side_lat_rel_speed = range(-5.0f, 5.0f);
    c->side_lon = range(-6.0f, 6.0f); c->side_lon_rel_speed = range(-10.0f, 5.0f);
    c->side_i_lead = (uint8_t)chance(30); c->slam_steer = range(-0.1f, 0.1f);
    c->side_valid = (uint8_t)chance(50); c->side_squeezed = (uint8_t)chance(15);
    c->contact_time = range(0.0f, 5.0f); c->contact_line_lat = range(-5.0f, 5.0f); c->pass_offset = range(-6.0f, 6.0f);
    c->contact_center_lat = range(-8.0f, 8.0f); c->contact_valid = (uint8_t)chance(40); c->contact_multiple = (uint8_t)chance(20);
    c->now = range(0.0f, 300.0f);
    c->speed = chance(90) ? sqrtf(c->velocity.x * c->velocity.x + c->velocity.y * c->velocity.y + c->velocity.z * c->velocity.z) : range(0.0f, 60.0f);
    c->track_half_width = range(3.0f, 12.0f);
    c->center_lat = chance(10) ? 0.0f : range(-c->track_half_width, c->track_half_width);
    c->center_lat_norm = c->center_lat / (c->track_half_width - 0.9525f) * range(0.8f, 1.2f);
    c->center_lat_delta = range(-0.3f, 0.3f);
    c->line_lat = range(-5.0f, 5.0f);
    c->finished = (uint8_t)(c->race_state == 3 ? 1 : chance(5));
    c->lap = rnd() % 5;
    c->race_begun = (uint8_t)chance(80);
    c->start_lat = range(-5.0f, 5.0f);
    int tk = rnd() % 4;
    c->tick = tk == 0 ? (rnd() & 0xff00) : tk == 1 ? (rnd() & 0xffc0) : tk == 2 ? (rnd() & 0xfff0) : (rnd() & 0xffff);
    if (chance(3)) c->tick = 0xffff;
    c->steer_damping = range(0.0f, 0.7f); c->steer = range(-1.0f, 1.0f); c->steer_gain = range(0.5f, 1.5f);
    c->tc_always = (uint8_t)chance(30);
    c->skill = (Skill*)g_skill;
    c->target_world.x = pos->x + range(-80.0f, 80.0f); c->target_world.z = pos->z + range(-80.0f, 80.0f);
    c->target_pos.x = range(-60.0f, 60.0f); c->target_pos.z = range(-30.0f, 100.0f);
    c->target_vel.x = range(-20.0f, 20.0f); c->target_vel.z = range(-10.0f, 80.0f);
    {
        const float l = sqrtf(c->target_pos.x * c->target_pos.x + c->target_pos.z * c->target_pos.z);
        if (chance(75) && l > 0.0f) { c->target_pos_dir.x = c->target_pos.x / l; c->target_pos_dir.z = c->target_pos.z / l; }
        else { c->target_pos_dir.x = range(-2.0f, 2.0f); c->target_pos_dir.z = range(-2.0f, 2.0f); }
        if (chance(5)) { c->target_pos_dir.x = 0.0f; c->target_pos_dir.z = 0.0f; }
    }
    c->target_vel_dir.x = range(-1.0f, 1.0f); c->target_vel_dir.z = chance(80) ? range(0.0f, 1.0f) : range(-1.0f, 0.0f);
    c->target_speed = chance(60) ? c->speed + range(-12.0f, 12.0f) : range(0.0f, 80.0f);
    c->center_pos.x = pos->x; c->center_pos.z = pos->z;
    c->center_recover_vel.x = range(-27.0f, 27.0f); c->center_recover_vel.z = range(-27.0f, 27.0f);
    c->center_tan.x = range(-1.0f, 1.0f); c->center_tan.z = range(-1.0f, 1.0f);
    c->edge_offset.x = range(-5.0f, 5.0f); c->edge_offset.z = range(-5.0f, 5.0f);
    c->side_edge_offset.x = range(-5.0f, 5.0f); c->side_edge_offset.z = range(-5.0f, 5.0f);
    c->center_right.x = range(-1.0f, 1.0f); c->center_right.z = range(-1.0f, 1.0f);
    const float now = c->now;
    c->progress_time = now - range(0.0f, 12.0f);
    c->finish_time = now - range(-5.0f, 40.0f);
    c->crash_time = now - range(0.0f, 4.0f);
    c->bump_time = now - range(0.0f, 4.0f);
    c->fouroff_time = now - range(0.0f, 15.0f);
    c->race_start_time = now - range(0.0f, 30.0f);
    c->freeze_time = now - range(0.0f, 10.0f);
    c->wall_heat = chance(30) ? range(40.0f, 80.0f) : range(0.0f, 50.0f);
    c->_fd8 = range(-1.0f, 1.0f);
    c->recover_side = chance(50) ? 1.0f : -1.0f;
    c->offground_cut = range(0.0f, 0.5f);
    c->line_update_time = now - range(0.0f, 1.0f);
    c->line = chance(96) ? (IdealLine*)g_line : 0;
    c->num_msgs = rnd() % 11;
    for (int i = 0; i < 10; i++) c->msgs[i] = *(const char**)k_msg_slots[rnd() % NMSG];
    if (chance(50)) { c->num_msgs = rnd() % 3; }
    c->min_brake = chance(70) ? 0.0f : uni();
    c->dice_cold_apex = (uint8_t)chance(50); c->dice_hot_entry = (uint8_t)chance(50); c->dice_unused = (uint8_t)chance(50);
    int tf = rnd() % 4;
    c->too_fast = tf == 0 ? 0.0f : tf == 1 ? range(0.0f, 0.1f) : range(0.0f, 1.0f);
    c->lap_target_time = range(50.0f, 120.0f); c->base_lap_time = chance(30) ? -1.0f : range(50.0f, 120.0f);
    c->lap_time_spread = range(0.0f, 3.0f); c->pace = range(0.8f, 1.2f); c->schedule_error = range(-5.0f, 5.0f);
    c->lap_clock = &g_lap_clock; g_lap_clock = range(0.0f, 120.0f);
    c->throttle_cap = range(0.2f, 1.0f); c->lap_time_offset = range(-2.0f, 5.0f); c->line_lap_time = range(50.0f, 120.0f);
    c->proxer_info = g_pinfo; c->num_proxer_info = rnd() % (NPINFO + 1);
    for (int i = 0; i < NPINFO; i++) { g_pinfo[i].level = range(0.0f, 2.0f); g_pinfo[i]._4 = rnd(); }
    c->learn_hiatus = chance(50) ? 0 : -3 + (int)(rnd() % 18);
    // ---- the racing line: a loop of nodes on a circle through the car
    g_nseg = 6 + rnd() % 20;
    const int N = g_nseg;
    const float cx = pos->x - R * cosf(a0), cz = pos->z - R * sinf(a0);
    memset(g_ring, 0, sizeof g_ring);
    for (int i = 0; i < N; i++) {
        ILSeg& s = g_ring[i];
        const float a = a0 + 6.2831853f * i / N + range(-0.02f, 0.02f);
        s.next = &g_ring[(i + 1) % N];
        s.pos.x = cx + R * cosf(a); s.pos.z = cz + R * sinf(a);
        s.dir.x = -sinf(a); s.dir.z = cosf(a);
        s.corridor_half_width = range(4.0f, 14.0f);
        s.target_speed = range(10.0f, 80.0f);
        s.curvature = 1.0f / R;
        s.step = 6.2831853f * R / N;
        s.cum_distance = s.step * i;
        s.index = (uint16_t)i;
        s.sector = (uint8_t)(i % 3);
        s.pad_ff = 0xff;
        s.field_11 = range(-1.0f, 1.0f);
        s.dist_ahead = chance(40) ? spd * range(0.0f, 2.5f) : range(-20.0f, 150.0f);
        s.dist_ahead_signed = chance(40) ? spd * range(0.0f, 3.0f) : range(-20.0f, 150.0f);
        s.cum_time = range(0.0f, 100.0f);
        s.lateral = range(-5.0f, 5.0f);
        s.pool_guard = 0x600dcafe;
    }
    memset(g_seginfo, 0, sizeof g_seginfo);
    for (int i = 0; i < N; i++) {
        SegmentInfo& si = g_seginfo[i];
        si.flag = (uint8_t)chance(25); si.offroad_left = (uint8_t)chance(20); si.offroad_right = (uint8_t)chance(20);
        si._03 = (uint8_t)rnd();
        si.speed_note = range(-5.0f, 5.0f);
        int lk = rnd() % 5;
        si.lateral_note = lk == 0 ? 0.0f : lk == 1 ? range(-0.15f, 0.15f) : lk == 2 ? range(-4.5f, 4.5f) : range(-2.0f, 2.0f);
        si.locked = (uint8_t)chance(20);
        si.lookahead = range(10.0f, 60.0f);
        si.seg = &g_ring[i];
    }
    c->seginfo = g_seginfo; c->num_segs = N;
    c->seg_index = rnd() % N; c->next_seg_index = chance(95) ? (c->seg_index + 1) % N : rnd() % N;
    c->seg_t = chance(5) ? 0.0f : uni();
    c->seg_changed = (uint8_t)chance(40); c->seg_valid = (uint8_t)chance(85);
    c->wall_hit_lat = range(-5.0f, 5.0f);
    c->last_learn_lap = c->lap - (int)(rnd() % 6);
    c->latslam_rel_speed = range(-5.0f, 5.0f);
    c->launch_time = range(0.0f, 5.0f);
    c->teleport_armed_time = chance(50) ? now - range(0.0f, 7.0f) : -100.0f;
    c->progress_seg = chance(50) ? &g_ring[c->seg_index] : &g_ring[rnd() % N];
    // ---- the skill
    memset(g_skill, 0, sizeof g_skill);
    for (int i = 0; i < SKILL_BYTES; i += 4) { float f = range(0.0f, 1.0f); memcpy(g_skill + i, &f, 4); }
    Skill* s = (Skill*)g_skill;
    s->recovery_time = range(0.0f, 3.0f);
    s->think_ticks = rnd() % 5;
    s->bump_tc_off_time = range(0.0f, 4.0f);
    s->aftershock_time = range(0.0f, 3.0f);
    s->throttle_scale = range(0.8f, 1.2f);
    s->steer_lock_time = range(0.3f, 1.5f);
    s->overspeed_throttle = range(-0.5f, 0.6f);
    s->hot_entry_threshold = (uint8_t)rnd(); s->cold_apex_threshold = (uint8_t)rnd(); s->dice3_threshold = (uint8_t)rnd();
    s->proxer_decay = range(0.9f, 1.0f);
    // ---- the lines
    memset(g_line, 0, sizeof g_line);
    IdealLine* L = (IdealLine*)g_line;
    L->vtable = (void**)0x004db6e8;
    L->bead_seg = &g_ring[c->seg_index]; L->bead_t = c->seg_t;
    L->car_pos.x = pos->x + range(-1.0f, 1.0f); L->car_pos.z = pos->z + range(-1.0f, 1.0f);
    memset(g_cline, 0, sizeof g_cline);
    CenterLine* C = (CenterLine*)g_cline;
    C->vtable = (void**)0x004db6f0;
    C->bead_seg = chance(85) ? &g_ring[rnd() % N] : 0;
    C->lat = c->center_lat + range(-0.5f, 0.5f);
    C->lat_delta = range(-0.3f, 0.3f);
    C->length = chance(97) ? 6.2831853f * R : chance(50) ? 0.0f : range(1.0f, 30.0f);
    // ---- the deity, which is also RaceDeity::global
    memset(g_deity_obj, 0, sizeof g_deity_obj);
    *(uint32_t*)g_deity_obj = 0x004dc588;
    for (int i = 0; i < 16; i++) {
        *(int32_t*)(g_deity_obj + 0x6c + 0x74 * i) = chance(70) ? c->lap : (int)(rnd() % 6);
        *(void**)(g_deity_obj + 0xa4 + 0x74 * i) = g_cline;
    }
    // ---- the proxer: up to 5 cars
    const int n = 1 + rnd() % 5;
    const int ci = rnd() % n;
    c->car_index = ci;
    memset(&g_prox, 0, sizeof g_prox);
    g_prox.num_objects = n;
    g_prox.record_size = n * 12 + 16;
    g_prox.records = g_records;
    memset(g_records, 0, sizeof g_records);
    const float fx = sinf(hdg), fz = cosf(hdg);
    for (int k = 0; k < 5; k++) {
        float* r = (float*)(g_records + g_prox.record_size * k);
        if (k == ci) { r[0] = pos->x; r[1] = pos->z; r[2] = c->velocity.x; r[3] = c->velocity.z; continue; }
        const float ahead = chance(60) ? range(-8.0f, 30.0f) : range(-60.0f, 60.0f), side = chance(60) ? range(-6.0f, 6.0f) : range(-20.0f, 20.0f);
        r[0] = pos->x + fx * ahead + fz * side; r[1] = pos->z + fz * ahead - fx * side;
        const float os = chance(15) ? -range(0.0f, 40.0f) : spd + range(-15.0f, 10.0f);
        r[2] = fx * os + range(-2.0f, 2.0f); r[3] = fz * os + range(-2.0f, 2.0f);
    }
    for (int k = 0; k < NDELTA; k++) {
        ProxerDelta& d = g_delta[k];
        memset(&d, 0, sizeof d);
        int other = n > 1 ? (ci + 1 + (int)(rnd() % (n - 1))) % n : (ci + 1) % 5;
        if (chance(50)) { d.car_a = (int16_t)ci; d.car_b = (int16_t)other; } else { d.car_a = (int16_t)other; d.car_b = (int16_t)ci; }
        const float* ra = (const float*)(g_records + g_prox.record_size * d.car_a);
        const float* rb = (const float*)(g_records + g_prox.record_size * d.car_b);
        d.dl.x = rb[0] - ra[0]; d.dl.z = rb[1] - ra[1];
        d.dv.x = rb[2] - ra[2]; d.dv.z = rb[3] - ra[3];
        if (chance(10)) { d.dl.x = range(-20.0f, 20.0f); d.dl.z = range(-20.0f, 20.0f); }
    }
    {   // my sorted delta list at record +0x10 + (2n + i) x 4
        uint8_t* rec = g_records + g_prox.record_size * ci;
        for (int i = 0; i < n; i++) *(ProxerDelta**)(rec + 0x10 + (2 * n + i) * 4) = chance(92) ? &g_delta[i] : 0;
    }
    // ---- globals
    for (int i = 0; i < TRACK_TABLE; i += 4) { float f = range(0.0f, 10.0f); memcpy(g_track_table + i, &f, 4); }
    *g_game_state_p = chance(60) ? 1 : chance(50) ? 2 : 4;
    *g_learn_p = (uint8_t)chance(20);
    *g_replay_play_p = (uint8_t)chance(20);
    *g_tick_p = chance(3) ? (int32_t)0x80000000 : (int32_t)(rnd() & 0x7fffff);
    // ---- the script
    for (int i = 0; i < 8; i++) g_script.random[i] = chance(10) ? (int)rnd() & 0x7fffffff : (int)(rnd() % 1000);
    g_script.time = chance(80) ? now + 0.016f * (rnd() % 3) : range(0.0f, 300.0f);
    g_script.dlong = range(0.0f, C->length > 0.0f ? C->length : 100.0f);
    g_script.m2i_seg = chance(97) ? &g_ring[rnd() % N] : 0; g_script.m2i_t = uni();
    g_script.bead_seg = chance(95) ? &g_ring[rnd() % N] : 0; g_script.bead_t = uni();
    g_script.near_seg = chance(50) ? &g_ring[0] : chance(60) ? &g_ring[rnd() % N] : 0;
    g_script.near_t = g_script.near_seg == &g_ring[0] ? range(0.0f, 0.15f) : uni(); g_script.near_dist = chance(5) ? 0.0f : range(0.0f, 80.0f);
    for (int i = 0; i < 16; i++) g_script.isai[i] = (uint8_t)chance(50);
    g_script.track = (int)(rnd() % 20);
    g_script.num_laps = 3 + rnd() % 6;
    g_script.cur_lap = rnd() % 8;
    g_script.teleport_ok = (uint8_t)chance(60);
}

enum Kind : int { K_UPDATE, K_BEGIN_RACE, K_BEGIN_LAP, K_RANDOM_REAL, K_ANALYZE, K_OFF_ROAD, K_DECAY, K_ROLL_DICE, K_LAPCHANGE,
    K_DONE_LOWERING, K_PERIODICS, K_INIT_RT_LAT, K_HERMITE, K_RIGHT, K_AFTERSHOCK, K_OBEY, K_SOCIALIZE, K_REV, K_YAW,
    K_HEADON, K_IN_PATH, K_FAST_INTERACT, K_CHECKER, K_PASSER, K_DONT_PUSH, K_DONT_CHECK, K_ADD_CONTACT, K_SLOW_INTERACT,
    K_LONSLAM, K_LATSLAM, K_FAST_DRIVE, K_MSGSIZE, K_ISSOLID, K_REPLAYSIZE, K_PINFO, N_KINDS };
static const char* const kind_names[N_KINDS] = {"AICar::Update", "AICar::begin_race", "AICar::begin_lap", "random_real",
    "AICar::analyze", "off_road", "AICar::decay", "AICar::roll_dice", "AICar::check_for_lapchange", "AICar::DoneLowering",
    "AICar::periodics", "AICar::init_rt_lat", "HermiteEval", "Right", "AICar::crash_aftershock", "AICar::obey_speed_limit",
    "AICar::socialize", "AICar::rev_engine", "AICar::apply_yaw_control", "AICar::headon_panic", "AICar::in_path",
    "AICar::fast_interact", "AICar::checker", "AICar::passer", "AICar::dont_push", "AICar::dont_check", "AICar::add_contact",
    "AICar::slow_interact", "AICar::lonslam", "AICar::latslam", "AICar::fast_drive", "Car::GetMessageSize", "Car::IsSolid",
    "AICar::GetReplayPacketSize", "AICar::ProxerInfo::ProxerInfo"};
static const int kind_weight[N_KINDS] = {30, 3, 3, 2, 10, 1, 2, 2, 2, 1, 10, 10, 2, 1, 3, 8, 6, 2, 5, 2, 4, 6, 10, 10, 3, 4,
    3, 1, 6, 8, 12, 1, 1, 1, 1};

static void randomize_args(Kind k) {
    AICar* c = car();
    for (int i = 0; i < 4; i++) g_args.brakes[i] = chance(30) ? 0.0f : range(-1.5f, 1.5f);
    g_args.throttle = chance(30) ? 1.0f : range(-0.5f, 1.2f);
    g_args.brake = chance(40) ? 0.0f : range(0.0f, 1.2f);
    g_args.steer = range(-1.2f, 1.2f);
    g_args.pos.seg = (ILSeg*)(uintptr_t)rnd(); g_args.pos.t = range(-1.0f, 2.0f);
    g_args.dist = range(-5.0f, 5.0f);
    const float* rec = (const float*)(g_records + g_prox.record_size * ((c->car_index + 1) % 5));
    g_args.p.x = chance(50) ? rec[0] : range(-100.0f, 100.0f); g_args.p.z = chance(50) ? rec[1] : range(-100.0f, 100.0f);
    if (k == K_RIGHT) { g_args.p.x = chance(10) ? wildf() : range(-100.0f, 100.0f); g_args.p.z = chance(10) ? wildf() : range(-100.0f, 100.0f); }
    g_args.hout.x = range(-1.0f, 1.0f); g_args.hout.z = range(-1.0f, 1.0f);
    for (int i = 0; i < 4; i++) { g_args.hin[i].x = range(-500.0f, 500.0f); g_args.hin[i].z = range(-500.0f, 500.0f); }
    g_args.ht = chance(10) ? (chance(50) ? 0.0f : 1.0f) : chance(10) ? wildf() : uni();
    // dont_check / dont_push / add_contact: lat, lat_v, lon, lon_v (and the contact's three)
    g_args.f[0] = fbits(chance(10) ? wildf() : range(-5.0f, 5.0f));
    g_args.f[1] = fbits(chance(10) ? wildf() : range(-6.0f, 6.0f));
    g_args.f[2] = fbits(chance(10) ? wildf() : range(-8.0f, 20.0f));
    g_args.f[3] = fbits(chance(10) ? wildf() : range(-10.0f, 10.0f));
    g_args.f[4] = fbits(range(-8.0f, 8.0f));
    if (k == K_DONT_PUSH || k == K_ADD_CONTACT) g_args.f[0] = fbits(chance(10) ? wildf() : range(0.0f, 20.0f));
    g_args.b = (uint8_t)chance(50);
    g_args.ra = chance(10) ? wildf() : range(-5.0f, 5.0f); g_args.rb = chance(10) ? wildf() : range(-5.0f, 5.0f);
    int nd = g_prox.num_objects - 1;
    g_args.delta = nd > 0 ? (int)(rnd() % nd) : 0;
    g_args.pi.level = range(0.0f, 1.0f); g_args.pi._4 = rnd();
}

// some floats replaced with extreme values
static void wildify(Kind kind) {
    uint8_t* b = g_carbuf;
    static const uint32_t car_floats[] = {0x234, 0x238, 0x23c, 0x240, 0x244, 0x248, 0x500, 0xc34, 0xd84, 0xd90, 0xdcc, 0xeb0,
        0xed4, 0xed8, 0xedc, 0xee0, 0xee4, 0xee8, 0xef0, 0xef8, 0xf0c, 0xf10, 0xf14, 0xf18, 0xf1c, 0xf20, 0xf64, 0xf68, 0xf74,
        0xf78, 0xf7c, 0xf80, 0xf84, 0xfb8, 0xfbc, 0xfc0, 0xfc4, 0xfd0, 0xfd4, 0xf48, 0xf50, 0x1018, 0x1020, 0x1028, 0x102c,
        0x1044, 0x1064, 0x106c, 0x107c};
    int n = 1 + rnd() % 4;
    for (int i = 0; i < n; i++) {
        uint32_t off = car_floats[rnd() % (sizeof car_floats / sizeof *car_floats)];
        float f = wildf();
        memcpy(b + off, &f, 4);
    }
    if (chance(30)) {                                    // a line node's float
        ILSeg& s = g_ring[rnd() % g_nseg];
        static const uint32_t seg_floats[] = {4, 8, 0xc, 0x10, 0x14, 0x30, 0x34, 0x3c};
        float f = wildf();
        memcpy((uint8_t*)&s + seg_floats[rnd() % 8], &f, 4);
    }
    if (chance(20)) { SegmentInfo& si = g_seginfo[rnd() % g_nseg]; float f = wildf(); memcpy(chance(50) ? &si.lateral_note : &si.speed_note, &f, 4); }
    if (chance(20)) { float f = wildf(); memcpy(g_skill + 4 * (rnd() % 26), &f, 4); if (((Skill*)g_skill)->think_ticks < 0 && chance(50)) ((Skill*)g_skill)->think_ticks = 3; }
    if (chance(15)) { CenterLine* C = (CenterLine*)g_cline; float f = wildf(); memcpy(chance(50) ? &C->lat : chance(50) ? &C->length : &C->lat_delta, &f, 4); }
    if (chance(15)) g_script.dlong = wildf();
    if (chance(15)) g_script.near_dist = wildf();
    if (chance(15)) { ProxerDelta& d = g_delta[rnd() % NDELTA]; float f = wildf(); memcpy((uint8_t*)&d + 4 + 4 * (rnd() % 4), &f, 4); }
    if (chance(15)) { float* r = (float*)(g_records + g_prox.record_size * (rnd() % 5)); r[rnd() % 4] = wildf(); }
    if (chance(20)) for (int i = 0; i < 4; i++) g_args.brakes[i] = chance(50) ? wildf() : g_args.brakes[i];
    if (chance(20)) g_args.brake = wildf();
    if (chance(20)) g_args.steer = wildf();
    // never the fptan range case (|steer| >= 2^63): there the original underflows its x87 stack
    AICar* c = car();
    if (kind == K_YAW && !(fabs((double)c->steering * 0.93 * c->max_steer_angle) < 4e18)) { c->steering = 0.5f; c->max_steer_angle = 0.5f; }
}

// ---- running both --------------------------------------------------------------------------------------------
struct State {
    uint8_t car[CAR_BUF];
    uint8_t skill[SKILL_BYTES];
    ILSeg ring[NSEG_MAX];
    SegmentInfo seginfo[NSEG_MAX];
    ProxerInfo pinfo[NPINFO];
    uint8_t line[60], cline[112], deity[DEITY_BYTES];
    Proxer prox;
    uint8_t records[NRECORDS];
    ProxerDelta delta[NDELTA];
    float lap_clock;
    Args args;
    uint8_t track[TRACK_TABLE];
    int32_t game_state, tick;
    uint8_t learn, replay_play;
    uint64_t ret;
    Log log;
};
static void save(State& s) {
    memcpy(s.car, g_carbuf, CAR_BUF);
    memcpy(s.skill, g_skill, sizeof g_skill);
    memcpy(s.ring, g_ring, sizeof g_ring);
    memcpy(s.seginfo, g_seginfo, sizeof g_seginfo);
    memcpy(s.pinfo, g_pinfo, sizeof g_pinfo);
    memcpy(s.line, g_line, sizeof g_line); memcpy(s.cline, g_cline, sizeof g_cline); memcpy(s.deity, g_deity_obj, sizeof g_deity_obj);
    s.prox = g_prox;
    memcpy(s.records, g_records, sizeof g_records);
    memcpy(s.delta, g_delta, sizeof g_delta);
    s.lap_clock = g_lap_clock;
    s.args = g_args;
    memcpy(s.track, g_track_table, TRACK_TABLE);
    s.game_state = *g_game_state_p; s.tick = *g_tick_p; s.learn = *g_learn_p; s.replay_play = *g_replay_play_p;
}
static void load(const State& s) {
    memcpy(g_carbuf, s.car, CAR_BUF);
    memcpy(g_skill, s.skill, sizeof g_skill);
    memcpy(g_ring, s.ring, sizeof g_ring);
    memcpy(g_seginfo, s.seginfo, sizeof g_seginfo);
    memcpy(g_pinfo, s.pinfo, sizeof g_pinfo);
    memcpy(g_line, s.line, sizeof g_line); memcpy(g_cline, s.cline, sizeof g_cline); memcpy(g_deity_obj, s.deity, sizeof g_deity_obj);
    g_prox = s.prox;
    memcpy(g_records, s.records, sizeof g_records);
    memcpy(g_delta, s.delta, sizeof g_delta);
    g_lap_clock = s.lap_clock;
    g_args = s.args;
    memcpy(g_track_table, s.track, TRACK_TABLE);
    *g_game_state_p = s.game_state; *g_tick_p = s.tick; *g_learn_p = s.learn; *g_replay_play_p = s.replay_play;
}

static uint64_t call_kind(Kind k, bool rw) {
    AICar* c = car();
    Args& a = g_args;
    uint64_t r = 0;
#define ORIG(T, A) ((T)(A))
    typedef void(__fastcall * M0)(AICar*, int);
    switch (k) {
    case K_UPDATE: rw ? AICar_Update(c, 0) : ORIG(M0, 0x00431010)(c, 0); break;
    case K_BEGIN_RACE: rw ? AICar_begin_race(c, 0) : ORIG(M0, 0x00430250)(c, 0); break;
    case K_BEGIN_LAP: rw ? AICar_begin_lap(c, 0) : ORIG(M0, 0x004302d0)(c, 0); break;
    case K_RANDOM_REAL: {
        double d = rw ? random_real(a.ra, a.rb) : ORIG(double(__cdecl*)(float, float), 0x00430350)(a.ra, a.rb);
        memcpy(&r, &d, 8);
        break;
    }
    case K_ANALYZE: rw ? AICar_analyze(c, 0) : ORIG(M0, 0x00430380)(c, 0); break;
    case K_OFF_ROAD: {
        Wheel* w = &c->wheels[a.delta & 3];
        r = rw ? off_road(w) : ORIG(uint8_t(__cdecl*)(Wheel*), 0x004305d0)(w);
        break;
    }
    case K_DECAY: rw ? AICar_decay(c, 0) : ORIG(M0, 0x004305e0)(c, 0); break;
    case K_ROLL_DICE: rw ? AICar_roll_dice(c, 0) : ORIG(M0, 0x00430630)(c, 0); break;
    case K_LAPCHANGE: rw ? AICar_check_for_lapchange(c, 0) : ORIG(M0, 0x004306a0)(c, 0); break;
    case K_DONE_LOWERING: rw ? AICar_DoneLowering(c, 0) : ORIG(M0, 0x004306e0)(c, 0); break;
    case K_PERIODICS: rw ? AICar_periodics(c, 0) : ORIG(M0, 0x004306f0)(c, 0); break;
    case K_INIT_RT_LAT: rw ? AICar_init_rt_lat(c, 0) : ORIG(M0, 0x004307e0)(c, 0); break;
    case K_HERMITE:
        rw ? HermiteEval(&a.hout, &a.hin[0], &a.hin[1], &a.hin[2], &a.hin[3], a.ht)
           : ORIG(void(__cdecl*)(Point2D*, const Point2D*, const Point2D*, const Point2D*, const Point2D*, float), 0x00430a70)(
                 &a.hout, &a.hin[0], &a.hin[1], &a.hin[2], &a.hin[3], a.ht);
        break;
    case K_RIGHT: rw ? Right(&a.p) : ORIG(void(__cdecl*)(Point2D*), 0x00430b20)(&a.p); break;
    case K_AFTERSHOCK: rw ? AICar_crash_aftershock(c, 0) : ORIG(M0, 0x00430b40)(c, 0); break;
    case K_OBEY:
        rw ? AICar_obey_speed_limit(c, 0, &a.throttle, &a.brake, &a.steer)
           : ORIG(void(__fastcall*)(AICar*, int, float*, float*, float*), 0x00430bc0)(c, 0, &a.throttle, &a.brake, &a.steer);
        break;
    case K_SOCIALIZE: rw ? AICar_socialize(c, 0) : ORIG(M0, 0x00430d90)(c, 0); break;
    case K_REV: rw ? AICar_rev_engine(c, 0) : ORIG(M0, 0x00430e20)(c, 0); break;
    case K_YAW: rw ? AICar_apply_yaw_control(c, 0, a.brakes) : ORIG(void(__fastcall*)(AICar*, int, float*), 0x00430e80)(c, 0, a.brakes); break;
    case K_HEADON: rw ? AICar_headon_panic(c, 0) : ORIG(M0, 0x004311d0)(c, 0); break;
    case K_IN_PATH: {
        const uint32_t md = fbits(c->speed * 4.0f);
        r = rw ? AICar_in_path(c, 0, &a.p, md, &a.pos, &a.dist)
               : ORIG(uint8_t(__fastcall*)(AICar*, int, const Point2D*, uint32_t, ILinePos*, float*), 0x00431240)(c, 0, &a.p, md, &a.pos, &a.dist);
        break;
    }
    case K_FAST_INTERACT: case K_CHECKER: case K_PASSER: case K_SLOW_INTERACT: {
        const ProxerDelta* d = &g_delta[a.delta];
        typedef uint8_t(__fastcall * I_t)(AICar*, int, const ProxerDelta*);
        if (k == K_FAST_INTERACT) r = rw ? AICar_fast_interact(c, 0, d) : ORIG(I_t, 0x004312a0)(c, 0, d);
        else if (k == K_CHECKER) r = rw ? AICar_checker(c, 0, d) : ORIG(I_t, 0x004312d0)(c, 0, d);
        else if (k == K_PASSER) r = rw ? AICar_passer(c, 0, d) : ORIG(I_t, 0x00431560)(c, 0, d);
        else r = rw ? AICar_slow_interact(c, 0, d) : ORIG(I_t, 0x00431950)(c, 0, d);
        break;
    }
    case K_DONT_PUSH:
        rw ? AICar_dont_push(c, 0, a.f[0], a.f[1]) : ORIG(void(__fastcall*)(AICar*, int, uint32_t, uint32_t), 0x00431800)(c, 0, a.f[0], a.f[1]);
        break;
    case K_DONT_CHECK:
        rw ? AICar_dont_check(c, 0, a.f[0], a.f[1], a.f[2], a.f[3], a.b)
           : ORIG(void(__fastcall*)(AICar*, int, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t), 0x00431840)(c, 0, a.f[0], a.f[1], a.f[2], a.f[3], a.b);
        break;
    case K_ADD_CONTACT:
        rw ? AICar_add_contact(c, 0, a.f[0], a.f[1], a.f[4])
           : ORIG(void(__fastcall*)(AICar*, int, uint32_t, uint32_t, uint32_t), 0x00431900)(c, 0, a.f[0], a.f[1], a.f[4]);
        break;
    case K_LONSLAM: rw ? AICar_lonslam(c, 0, &a.brake) : ORIG(void(__fastcall*)(AICar*, int, float*), 0x00431970)(c, 0, &a.brake); break;
    case K_LATSLAM:
        rw ? AICar_latslam(c, 0, &a.brake, &a.steer) : ORIG(void(__fastcall*)(AICar*, int, float*, float*), 0x00431a90)(c, 0, &a.brake, &a.steer);
        break;
    case K_FAST_DRIVE: rw ? AICar_fast_drive(c, 0) : ORIG(M0, 0x00431ce0)(c, 0); break;
    case K_MSGSIZE: r = (uint32_t)(rw ? Car_GetMessageSize(c, 0) : ORIG(int(__fastcall*)(void*, int), 0x00432060)(c, 0)); break;
    case K_ISSOLID: r = rw ? Car_IsSolid((CarOpacity*)c, 0) : ORIG(uint8_t(__fastcall*)(void*, int), 0x00432070)(c, 0); break;
    case K_REPLAYSIZE: r = (uint32_t)(rw ? AICar_GetReplayPacketSize(c, 0) : ORIG(int(__fastcall*)(void*, int), 0x00432080)(c, 0)); break;
    case K_PINFO: {
        ProxerInfo* p = rw ? ProxerInfo_ProxerInfo(&a.pi, 0) : ORIG(ProxerInfo*(__fastcall*)(ProxerInfo*, int), 0x00432090)(&a.pi, 0);
        r = (uint32_t)p;
        break;
    }
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
    g_random_i = 0;
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    int faulted = 0;
    __try { *ret = call_kind(k, rewrite); } __except (fault_filter(GetExceptionInformation())) { faulted = 1; }
    unsigned short fcw = 0x027f;                          // the x87 back to a clean state after a fault
    if (faulted) __asm { fninit
                         fldcw fcw }
    return faulted;
}

// the rewrite's footprint for the world's call
static void footprint_of(Kind k, Footprint& fp) {
    AICar* c = car();
    Args& a = g_args;
    fp.n = 0; fp.pure = false; fp.replay_only = 0;
    switch (k) {
    case K_UPDATE: fpof_AICar_Update(fp, c, 0); break;
    case K_BEGIN_RACE: fpof_AICar_begin_race(fp, c, 0); break;
    case K_BEGIN_LAP: fpof_AICar_begin_lap(fp, c, 0); break;
    case K_RANDOM_REAL: fpof_random_real(fp, a.ra, a.rb); break;
    case K_ANALYZE: fpof_AICar_analyze(fp, c, 0); break;
    case K_OFF_ROAD: fpof_off_road(fp, &c->wheels[a.delta & 3]); break;
    case K_DECAY: fpof_AICar_decay(fp, c, 0); break;
    case K_ROLL_DICE: fpof_AICar_roll_dice(fp, c, 0); break;
    case K_LAPCHANGE: fpof_AICar_check_for_lapchange(fp, c, 0); break;
    case K_DONE_LOWERING: fpof_AICar_DoneLowering(fp, c, 0); break;
    case K_PERIODICS: fpof_AICar_periodics(fp, c, 0); break;
    case K_INIT_RT_LAT: fpof_AICar_init_rt_lat(fp, c, 0); break;
    case K_HERMITE: fpof_HermiteEval(fp, &a.hout, &a.hin[0], &a.hin[1], &a.hin[2], &a.hin[3], a.ht); if (fp.pure) fp.add(&a.hout, 8, "out"); break;
    case K_RIGHT: fpof_Right(fp, &a.p); if (fp.pure) fp.add(&a.p, 8, "p"); break;
    case K_AFTERSHOCK: fpof_AICar_crash_aftershock(fp, c, 0); break;
    case K_OBEY: fpof_AICar_obey_speed_limit(fp, c, 0, &a.throttle, &a.brake, &a.steer); break;
    case K_SOCIALIZE: fpof_AICar_socialize(fp, c, 0); break;
    case K_REV: fpof_AICar_rev_engine(fp, c, 0); break;
    case K_YAW: fpof_AICar_apply_yaw_control(fp, c, 0, a.brakes); break;
    case K_HEADON: fpof_AICar_headon_panic(fp, c, 0); break;
    case K_IN_PATH: fpof_AICar_in_path(fp, c, 0, &a.p, 0, &a.pos, &a.dist); break;
    case K_FAST_INTERACT: fpof_AICar_fast_interact(fp, c, 0, &g_delta[a.delta]); break;
    case K_CHECKER: fpof_AICar_checker(fp, c, 0, &g_delta[a.delta]); break;
    case K_PASSER: fpof_AICar_passer(fp, c, 0, &g_delta[a.delta]); break;
    case K_DONT_PUSH: fpof_AICar_dont_push(fp, c, 0, a.f[0], a.f[1]); break;
    case K_DONT_CHECK: fpof_AICar_dont_check(fp, c, 0, a.f[0], a.f[1], a.f[2], a.f[3], a.b); break;
    case K_ADD_CONTACT: fpof_AICar_add_contact(fp, c, 0, a.f[0], a.f[1], a.f[4]); break;
    case K_SLOW_INTERACT: fpof_AICar_slow_interact(fp, c, 0, &g_delta[a.delta]); break;
    case K_LONSLAM: fpof_AICar_lonslam(fp, c, 0, &a.brake); break;
    case K_LATSLAM: fpof_AICar_latslam(fp, c, 0, &a.brake, &a.steer); break;
    case K_FAST_DRIVE: fpof_AICar_fast_drive(fp, c, 0); break;
    case K_MSGSIZE: fpof_Car_GetMessageSize(fp, c, 0); break;
    case K_ISSOLID: fpof_Car_IsSolid(fp, (CarOpacity*)c, 0); break;
    case K_REPLAYSIZE: fpof_AICar_GetReplayPacketSize(fp, c, 0); break;
    case K_PINFO: fpof_ProxerInfo_ProxerInfo(fp, &a.pi, 0); break;
    default: break;
    }
}
static bool in_footprint(const Footprint& fp, const uint8_t* p) {
    for (int i = 0; i < fp.n; i++)
        if (p >= (const uint8_t*)fp.r[i].p && p < (const uint8_t*)fp.r[i].p + fp.r[i].n) return true;
    return false;
}

// a name for a byte of the AICar
static const char* name_of(uint32_t off, char* buf) {
    struct F { uint32_t off, size; const char* name; };
    static const F fields[] = {
        {0x234, 12, "velocity"}, {0x240, 12, "angular_velocity"}, {0x4f8, 4, "yaw_control"}, {0x500, 4, "steering"},
        {0x554, 4 * 0x1a4, "wheels"}, {0xde0, 4, "throttle"}, {0xde4, 4, "braking"}, {0xde8, 4, "ebrake"}, {0xdec, 4, "clutch"},
        {0xe3d, 1, "lowering"}, {0xed0, 1, "ahead_valid"}, {0xed4, 4, "ahead_dist"}, {0xed8, 4, "ahead_rel_speed"},
        {0xedc, 4, "side_lat"}, {0xee0, 4, "side_lat_rel_speed"}, {0xee4, 4, "side_lon"}, {0xee8, 4, "side_lon_rel_speed"},
        {0xeec, 1, "side_i_lead"}, {0xef0, 4, "slam_steer"}, {0xef4, 1, "side_valid"}, {0xef5, 1, "side_squeezed"},
        {0xef8, 4, "contact_time"}, {0xefc, 4, "contact_line_lat"}, {0xf04, 4, "contact_center_lat"}, {0xf08, 1, "contact_valid"},
        {0xf09, 1, "contact_multiple"}, {0xf0c, 4, "now"}, {0xf10, 4, "speed"}, {0xf14, 4, "track_half_width"},
        {0xf18, 4, "center_lat"}, {0xf1c, 4, "center_lat_norm"}, {0xf20, 4, "center_lat_delta"}, {0xf24, 4, "line_lat"},
        {0xf34, 4, "lap"}, {0xf38, 1, "race_begun"}, {0xf3c, 4, "start_lat"}, {0xf40, 4, "tick"}, {0xf48, 4, "steer_damping"},
        {0xf4c, 4, "steer"}, {0xf88, 8, "center_pos"}, {0xf90, 8, "center_recover_vel"}, {0xf98, 8, "center_tan"},
        {0xfa0, 8, "edge_offset"}, {0xfa8, 8, "side_edge_offset"}, {0xfb0, 8, "center_right"}, {0xfb8, 4, "progress_time"},
        {0xfc0, 4, "crash_time"}, {0xfc8, 4, "fouroff_time"}, {0xfcc, 4, "race_start_time"}, {0xfd0, 4, "freeze_time"},
        {0xfd4, 4, "wall_heat"}, {0xfdc, 4, "recover_side"}, {0xfe0, 4, "offground_cut"}, {0xfec, 40, "msgs"},
        {0x1014, 4, "num_msgs"}, {0x101c, 3, "dice"}, {0x1020, 4, "too_fast"}, {0x1024, 4, "lap_target_time"},
        {0x1030, 4, "pace"}, {0x1034, 4, "schedule_error"}, {0x103c, 4, "throttle_cap"}, {0x1040, 4, "lap_time_offset"},
        {0x1050, 4, "learn_hiatus"}, {0x105c, 4, "seg_index"}, {0x1060, 4, "next_seg_index"}, {0x1064, 4, "seg_t"},
        {0x1068, 2, "seg_changed/valid"}, {0x1070, 4, "last_learn_lap"}, {0x1074, 4, "latslam_rel_speed"},
        {0x107c, 4, "teleport_armed_time"}};
    for (const F& f : fields)
        if (off >= f.off && off < f.off + f.size) {
            if (f.off == 0x554) sprintf(buf, "wheels[%u]+0x%x", (off - 0x554) / 0x1a4, (off - 0x554) % 0x1a4);
            else sprintf(buf, "car.%s+%u", f.name, off - f.off);
            return buf;
        }
    sprintf(buf, "car+0x%x", off);
    return buf;
}

static void compare_block(const uint8_t* o, const uint8_t* n, uint32_t size, const char* what, bool& same, int it, Kind k, bool is_car) {
    char buf[96];
    int shown = 0;
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t a, b;
        memcpy(&a, o + i, 4); memcpy(&b, n + i, 4);
        if (a == b) continue;
        if (same) printf("  MISMATCH, world %d (%s)\n", it, kind_names[k]);
        same = false;
        if (++shown > 12) continue;
        if (is_car) printf("    %-34s original %08x (%.9g)  rewrite %08x (%.9g)\n", name_of(i, buf), a, bitsf(a), b, bitsf(b));
        else printf("    %s+0x%x original %08x (%.9g)  rewrite %08x (%.9g)\n", what, i, a, bitsf(a), b, bitsf(b));
    }
}
static bool compare(const State& o, const State& n, int it, Kind k) {
    bool same = true;
    compare_block(o.car, n.car, CAR_BUF, "car", same, it, k, true);
    compare_block(o.skill, n.skill, sizeof o.skill, "skill", same, it, k, false);
    compare_block((const uint8_t*)o.ring, (const uint8_t*)n.ring, sizeof o.ring, "ring", same, it, k, false);
    compare_block((const uint8_t*)o.seginfo, (const uint8_t*)n.seginfo, sizeof o.seginfo, "seginfo", same, it, k, false);
    compare_block((const uint8_t*)o.pinfo, (const uint8_t*)n.pinfo, sizeof o.pinfo, "proxer info", same, it, k, false);
    compare_block(o.line, n.line, sizeof o.line, "line", same, it, k, false);
    compare_block(o.cline, n.cline, sizeof o.cline, "centre line", same, it, k, false);
    compare_block(o.deity, n.deity, sizeof o.deity, "deity", same, it, k, false);
    compare_block((const uint8_t*)&o.prox, (const uint8_t*)&n.prox, sizeof o.prox, "proxer", same, it, k, false);
    compare_block(o.records, n.records, sizeof o.records, "records", same, it, k, false);
    compare_block((const uint8_t*)o.delta, (const uint8_t*)n.delta, sizeof o.delta, "deltas", same, it, k, false);
    compare_block((const uint8_t*)&o.args, (const uint8_t*)&n.args, sizeof o.args, "args", same, it, k, false);
    compare_block(o.track, n.track, sizeof o.track, "track table", same, it, k, false);
    if (memcmp(&o.lap_clock, &n.lap_clock, 4) || o.game_state != n.game_state || o.tick != n.tick || o.learn != n.learn ||
        o.replay_play != n.replay_play) {
        if (same) printf("  MISMATCH, world %d (%s)\n", it, kind_names[k]);
        printf("    a global differs\n");
        same = false;
    }
    if (o.ret != n.ret) {
        if (same) printf("  MISMATCH, world %d (%s)\n", it, kind_names[k]);
        printf("    return: original %llx rewrite %llx\n", o.ret, n.ret);
        same = false;
    }
    if (o.log.n != n.log.n || memcmp(o.log.e, n.log.e, 4 * (o.log.n < 256 ? o.log.n : 256))) {
        if (same) printf("  MISMATCH, world %d (%s)\n", it, kind_names[k]);
        printf("    the stub logs differ: %d / %d entries\n", o.log.n, n.log.n);
        int m = o.log.n < n.log.n ? o.log.n : n.log.n;
        if (m > 256) m = 256;
        for (int i = 0; i < m; i++) if (o.log.e[i] != n.log.e[i]) { printf("    first at %d: %08x / %08x\n", i, o.log.e[i], n.log.e[i]); break; }
        same = false;
    }
    return same;
}

// the bytes the original changed, against the rewrite's footprint (a change outside it is reported once a world)
static int check_footprint(const State& start, const State& orig, const Footprint& fp, int it, Kind k) {
    if (fp.replay_only) return 0;
    struct R { const uint8_t *a, *b, *live; uint32_t n; const char* what; } regions[] = {
        {start.car, orig.car, g_carbuf, CAR_BUF, "car"},
        {start.skill, orig.skill, g_skill, sizeof g_skill, "skill"},
        {(const uint8_t*)start.ring, (const uint8_t*)orig.ring, (const uint8_t*)g_ring, sizeof g_ring, "ring"},
        {(const uint8_t*)start.seginfo, (const uint8_t*)orig.seginfo, (const uint8_t*)g_seginfo, sizeof g_seginfo, "seginfo"},
        {(const uint8_t*)start.pinfo, (const uint8_t*)orig.pinfo, (const uint8_t*)g_pinfo, sizeof g_pinfo, "proxer info"},
        {start.line, orig.line, g_line, sizeof g_line, "line"},
        {start.cline, orig.cline, g_cline, sizeof g_cline, "centre line"},
        {start.deity, orig.deity, g_deity_obj, sizeof g_deity_obj, "deity"},
        {start.records, orig.records, g_records, sizeof g_records, "records"},
        {(const uint8_t*)start.delta, (const uint8_t*)orig.delta, (const uint8_t*)g_delta, sizeof g_delta, "deltas"},
        {(const uint8_t*)&start.args, (const uint8_t*)&orig.args, (const uint8_t*)&g_args, sizeof g_args, "args"},
        {(const uint8_t*)&start.game_state, (const uint8_t*)&orig.game_state, (const uint8_t*)g_game_state_p, 4, "game state"}};
    for (const R& r : regions)
        for (uint32_t i = 0; i < r.n; i++)
            if (r.a[i] != r.b[i] && !in_footprint(fp, r.live + i)) {
                char buf[96];
                printf("  FOOTPRINT, world %d (%s): %s+0x%x changed outside it%s%s\n", it, kind_names[k], r.what, i,
                       r.live == g_carbuf ? " = " : "", r.live == g_carbuf ? name_of(i, buf) : "");
                return 1;
            }
    return 0;
}

// the _MSG reasons a run added (bit per msg_tab slot)
static uint32_t msgs_added(const State& start, const State& end) {
    const AICar* s = (const AICar*)start.car;
    const AICar* e = (const AICar*)end.car;
    uint32_t m = 0;
    for (int i = 0; i < e->num_msgs && i < 10; i++) {
        bool old = false;
        for (int j = 0; j < s->num_msgs && j < 10 && e->num_msgs >= s->num_msgs; j++) if (s->msgs[j] == e->msgs[i]) old = true;
        if (old && e->num_msgs >= s->num_msgs) continue;
        for (int k = 0; k < NMSG; k++) if (*(const char**)k_msg_slots[k] == e->msgs[i]) m |= 1u << k;
    }
    return m;
}

#ifdef FIX_TESTS
// ==== the fix tests ================================================================================================
static int g_fix_fail;
#define FIXCHECK(cond, ...)                                                                                     \
    do {                                                                                                        \
        if (!(cond)) {                                                                                          \
            if (g_fix_fail++ < 30) { printf("FIX FAIL: "); printf(__VA_ARGS__); printf("\n"); }                 \
        }                                                                                                       \
    } while (0)
static bool log_has(const Log& l, uint32_t v) { for (int i = 0; i < l.n && i < 256; i++) if (l.e[i] == v) return true; return false; }
static void run_fix_tests() {
    static State a, ra, b;
    long same = 0, not_found = 0, orig_faults = 0;
    // init_rt_lat on a lost bead: the fixed one asks reset_bead_position (the stub puts it at a ring node) and goes on
    // as the original does on a bead already there
    for (int it = 0; it < 20000; it++) {
        randomize_world();
        randomize_args(K_INIT_RT_LAT);
        AICar* c = car();
        c->line = (IdealLine*)g_line;
        const int k = (int)(rnd() % g_nseg);
        const float t = uni();
        g_script.m2i_seg = &g_ring[rnd() % g_nseg];              // (a centre line: its absence is another crash)
        g_script.bead_seg = 0;
        g_script.bead_t = t;
        g_reset_bead_to = chance(15) ? -1 : k;
        save(a);
        uint64_t r;
        const int fn = run_guarded(K_INIT_RT_LAT, true, &r);
        save(ra);
        ra.log = g_log;
        FIXCHECK(!fn, "init_rt_lat faulted on a NULL bead (world %d)", it);
        FIXCHECK(log_has(ra.log, 0x4e5e0000u), "init_rt_lat didn't ask for the bead (world %d)", it);
        g_script.bead_seg = 0;
        load(a);
        const int fo = run_guarded(K_INIT_RT_LAT, false, &r);
        if (fo) orig_faults++;
        if (g_reset_bead_to < 0) {
            not_found++;
            FIXCHECK(!memcmp(&((AICar*)ra.car)->line_lat, &((AICar*)a.car)->line_lat, 4), "no bead: line_lat changed");
            g_reset_bead_to = -1;
            continue;
        }
        // the original, on the bead where reset_bead_position put it
        g_reset_bead_to = -1;
        load(a);
        g_script.bead_seg = &g_ring[k];
        const int fb = run_guarded(K_INIT_RT_LAT, false, &r);
        save(b);
        if (fb) continue;
        same++;
        FIXCHECK(!memcmp(ra.car, b.car, CAR_BUF), "init_rt_lat: the found bead's result differs from the original's on it (world %d)", it);
    }
    g_reset_bead_to = -1;
    printf("init_rt_lat, NULL bead: the original faulted %ld times; the fixed one found it (%ld as the original's on it) or not "
           "(%ld, line_lat kept)\n", orig_faults, same, not_found);
    FIXCHECK(orig_faults > 10000, "the original didn't fault on NULL beads");
    // begin_race: long driver names (the original's strcpy would run over its stack: it isn't run on them)
    static char name[8192];
    long long_names = 0;
    for (int it = 0; it < 4000; it++) {
        randomize_world();
        randomize_args(K_BEGIN_RACE);
        *g_replay_play_p = 0;
        save(a);
        g_driver_name = "Doug";
        uint64_t r;
        const int fo = run_guarded(K_BEGIN_RACE, false, &r);
        save(b);
        b.log = g_log;
        load(a);
        const int kind = it % 5;
        const int len = kind == 0 ? (int)(rnd() % 32) : 32 + (int)(rnd() % 8000);
        for (int i = 0; i < len; i++) name[i] = (char)('a' + rnd() % 26);
        name[len] = 0;
        g_driver_name = kind == 4 && chance(20) ? 0 : name;
        const int fn = run_guarded(K_BEGIN_RACE, true, &r);
        save(ra);
        ra.log = g_log;
        g_driver_name = "Doug";
        FIXCHECK(!fo && !fn, "begin_race faulted (original %d, fixed %d)", fo, fn);
        FIXCHECK(!memcmp(ra.car, b.car, CAR_BUF), "begin_race: the car differs with a %d-character name", len);
        if (kind == 0) FIXCHECK(log_has(ra.log, 0xd5a30000u), "begin_race: a short name not fetched");
        else {
            long_names++;
            FIXCHECK(!log_has(ra.log, 0xd5a30000u), "begin_race: a %d-character name went to AIGetDriverNameByCar", len);
        }
    }
    printf("begin_race: %ld names of 32 characters or more (or NULL) cut, none faulted; short ones fetched as before\n", long_names);
}
#endif

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 200000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const int only = argc > 3 ? atoi(argv[3]) : -1;
    if (argc > 4) g_wild_pct = atoi(argv[4]);
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_aicar2.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    patch_jmp(0x0041b6e0, (void*)&stub_random);
    patch_jmp(0x0042bc80, (void*)&stub_time);
    patch_jmp(0x00420d40, (void*)&stub_driver_name);
    patch_jmp(0x00420df0, (void*)&stub_track_number);
    patch_jmp(0x0041d370, (void*)&stub_isai);
    patch_jmp(0x00421c10, (void*)&stub_bead);
    patch_jmp(0x004216f0, (void*)&stub_nearest);
    patch_jmp(0x004214c0, (void*)&stub_update_car_info);
    patch_jmp(0x00421520, (void*)&stub_reset_bead);
    patch_jmp(0x004225e0, (void*)&stub_dlong);
    patch_jmp(0x00422710, (void*)&stub_m2i);
    patch_jmp(0x004300f0, (void*)&stub_real_target);
    patch_jmp(0x00437680, (void*)&stub_car_update);
    patch_jmp(0x00439760, (void*)&stub_gear_auto_ai);
    // the deity: RaceDeity's real vtable (so the footprint accepts it), its slots patched to the stubs
    patch_jmp(0x0042c5e0, (void*)&stub_stop_race);       // slot 8 (Deity::StopRace)
    patch_jmp(0x004441e0, (void*)&stub_num_laps);        // slot 0x20
    patch_jmp(0x00443a00, (void*)&stub_teleport);        // slot 0x3c
    patch_jmp(0x004441c0, (void*)&stub_cur_lap);         // slot 0x48
    *(void**)0x005218ac = g_deity_obj;
    *(void**)0x004edd70 = g_deity_obj;                   // RaceDeity::global
    *(Proxer**)0x004ec480 = &g_prox;
    *(void**)0x00521f7c = 0;                             // no splash sound
#ifdef FIX_TESTS
    *(void**)0x004eb8b0 = &g_fake_lounge;                // (begin_race's fixed lookup: AIDriverLounge::Get)
#endif
    *(void**)0x004e642c = (void*)&stub_ov_begin;
    *(void**)0x004e6430 = (void*)&stub_ov_end;
    *(void**)0x004e6434 = (void*)&stub_prof_start;
    *(void**)0x004e6438 = (void*)&stub_prof_stop;
    g_carbuf = (uint8_t*)VirtualAlloc(0, CAR_BUF, MEM_COMMIT, PAGE_READWRITE);
    g_vrmod = getenv("VP_VRMOD") && atoi(getenv("VP_VRMOD"));
    if (g_vrmod) {
        vrmod_apply();
        puts("vrmod's race.exe, and headon.py's ret in AICar::headon_panic in half the worlds");
    }
    int headon_worlds = 0, headon_rets = 0, headon_panics = 0;

    static State start, orig, rew;
    static Footprint fp;
    int differ = 0, faults = 0, wild_runs = 0, wild_differ = 0, fp_bad = 0, replay_only = 0;
    int per_kind[N_KINDS] = {0}, per_kind_bad[N_KINDS] = {0}, per_kind_fault[N_KINDS] = {0};
    uint32_t per_kind_msgs[N_KINDS] = {0};
    int c_think_drive = 0, c_rev = 0, c_begin = 0, c_teleport = 0, c_stop = 0, c_side = 0, c_ahead = 0, c_contact = 0, c_headon = 0,
        c_lock = 0, c_speed_note = 0, c_lat_note = 0, c_cold = 0, c_angle = 0, c_lock_steer = 0, c_cap = 0, c_lap = 0,
        c_fouroff = 0, c_fmod_nan = 0, c_near_null = 0;
    int fixed = 0;                                        // (FIX_TESTS: the original faulted, the fixed one ran)
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weight[i];
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), k = 0;
        while (pick >= kind_weight[k]) pick -= kind_weight[k++];
        Kind kind = (Kind)(only >= 0 ? only : k);
        randomize_world();
        randomize_args(kind);
        if (g_vrmod) {                                  // headon.py: `ret` in place of `sub esp, 4` (0x83), or not
            g_headon = chance(50);
            *(uint8_t*)0x004311d0 = g_headon ? 0xc3 : 0x83;
            if (g_headon) headon_worlds++;
        }
        bool wild = chance(g_wild_pct);
        if (wild) { wildify(kind); wild_runs++; }
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
#ifdef FIX_TESTS
        // the fixed rewrite: its changes inside its footprint; a lost bead in init_rt_lat is the fix
        fp_bad += check_footprint(start, rew, fp, it, kind);
        if (fo && !fn && kind == K_INIT_RT_LAT && car()->line && !g_script.bead_seg) { fixed++; continue; }
#endif
        if (fo || fn) {
            per_kind_fault[kind]++;
            if (fo != fn) {
                printf("  world %d (%s): original %s, rewrite %s (fault %08x at %08x, address %08x)\n", it, kind_names[kind],
                       fo ? "faulted" : "ran", fn ? "faulted" : "ran", g_fault_code, g_fault_eip, g_fault_addr);
                differ++; per_kind_bad[kind]++;
                if (differ >= 20) { printf("stopping after 20 differing worlds\n"); break; }
            }
            faults++;
            continue;
        }
        if (!compare(orig, rew, it, kind)) {
            differ++;
            per_kind_bad[kind]++;
            if (wild) wild_differ++;
            if (differ >= 20) { printf("stopping after 20 differing worlds\n"); break; }
        }
        fp_bad += check_footprint(start, orig, fp, it, kind);
        // coverage, from the starting state, the original's result and its log
        const AICar* sc = (const AICar*)start.car;
        const AICar* oc = (const AICar*)orig.car;
        per_kind_msgs[kind] |= msgs_added(start, orig);
        for (int i = 0; i < orig.log.n && i < 256; i++) {
            if (orig.log.e[i] == 0x7a400000u) c_think_drive++;
            if (orig.log.e[i] == 0xde17003cu) c_teleport++;
            if (orig.log.e[i] == 0xde170008u) c_stop++;
        }
        if (kind == K_UPDATE && sc->race_state < 2) c_rev++;
        if (kind == K_UPDATE && !sc->race_begun) c_begin++;
        if ((kind == K_CHECKER || kind == K_FAST_INTERACT || kind == K_SOCIALIZE) && oc->side_valid && memcmp(&sc->side_lat, &oc->side_lat, 16)) c_side++;
        if ((kind == K_CHECKER || kind == K_FAST_INTERACT || kind == K_SOCIALIZE) && oc->ahead_valid && memcmp(&sc->ahead_dist, &oc->ahead_dist, 8)) c_ahead++;
        if ((kind == K_PASSER || kind == K_FAST_INTERACT || kind == K_SOCIALIZE) && oc->contact_valid && memcmp(&sc->contact_time, &oc->contact_time, 4)) c_contact++;
        if ((kind == K_PASSER || kind == K_FAST_INTERACT) && memcmp(&sc->freeze_time, &oc->freeze_time, 4)) c_headon++;
        if (kind == K_HEADON && g_headon) {
            headon_rets++;
            if (memcmp(start.car, orig.car, sizeof start.car)) {   // vrmod's ret: the original must have done nothing
                printf("  world %d: headon_panic changed the car under headon.py's ret\n", it);
                differ++;
            }
        } else if (kind == K_HEADON && memcmp(start.car, orig.car, sizeof start.car)) {
            headon_panics++;
        }
        if (kind == K_ANALYZE) {
            for (int i = 0; i < g_nseg; i++) {
                if (!start.seginfo[i].locked && orig.seginfo[i].locked) c_lock++;
                if (memcmp(&start.seginfo[i].speed_note, &orig.seginfo[i].speed_note, 4)) c_speed_note++;
                if (memcmp(&start.seginfo[i].lateral_note, &orig.seginfo[i].lateral_note, 4)) c_lat_note++;
            }
        }
        if (kind == K_FAST_DRIVE) {
            if (msgs_added(start, orig) & (1u << 10)) c_cold++;
            if (fbits(sc->target_vel_dir.z) - 1u < 0x7fffffffu) c_angle++; else c_lock_steer++;
            if (sc->finished) c_cap++;
        }
        if (kind == K_LAPCHANGE && sc->lap != oc->lap) c_lap++;
        if (kind == K_PERIODICS && memcmp(&sc->fouroff_time, &oc->fouroff_time, 4)) c_fouroff++;
        if (kind == K_INIT_RT_LAT && !(((const CenterLine*)start.cline)->length > 0.0f)) c_fmod_nan++;
        if (kind == K_IN_PATH && !g_script.near_seg) c_near_null++;
    }
    printf("%d worlds (%d wild): %d differ (%d of them wild), %d faulted in both, %d changed bytes outside the footprint, %d replay-only\n",
           iterations, wild_runs, differ, wild_differ, faults, fp_bad, replay_only);
    printf("per function (worlds / differing / faulted in both, _MSG reasons seen):\n");
    for (int i = 0; i < N_KINDS; i++) {
        printf("  %-34s %6d / %d / %d", kind_names[i], per_kind[i], per_kind_bad[i], per_kind_fault[i]);
        if (per_kind_msgs[i]) {
            printf("  [");
            for (int m = 0; m < NMSG; m++) if (per_kind_msgs[i] & (1u << m)) printf(" %s", *(const char**)k_msg_slots[m]);
            printf(" ]");
        }
        printf("\n");
    }
    printf("covered: think+drive %d, rev_engine %d, begin_race %d, teleports %d, StopRace %d, side car taken %d, car ahead taken %d,\n"
           "  contact %d, head-on %d, notes locked %d / speed notes %d / lateral notes %d, fast_drive cold apex %d, bearing %d / lock %d,\n"
           "  throttle cap %d, lap change %d, fouroff %d, init_rt_lat fmod of a bad length %d, in_path NULL bead %d\n",
           c_think_drive, c_rev, c_begin, c_teleport, c_stop, c_side, c_ahead, c_contact, c_headon, c_lock, c_speed_note, c_lat_note,
           c_cold, c_angle, c_lock_steer, c_cap, c_lap, c_fouroff, c_fmod_nan, c_near_null);
    if (g_vrmod)
        printf("vrmod: headon.py's ret in %d worlds (%d of them calls of headon_panic: nothing done, by both); the stock "
               "panic in %d calls without it\n", headon_worlds, headon_rets, headon_panics);
#ifdef FIX_TESTS
    printf("fixed: %d worlds where the original faulted on a NULL bead and the fixed one ran\n", fixed);
    run_fix_tests();
    printf("fix tests: %d failures\n", g_fix_fail);
    if (g_fix_fail) differ++;
#else
    (void)fixed;
#endif
    return differ || fp_bad ? 1 : 0;
}
