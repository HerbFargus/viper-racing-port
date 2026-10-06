// world_aicar1.cpp -- the world harness for the AI driver's first half (hook/phys_aicar1.cpp): the originals and
// the rewrites, run on the same random AICars, compared byte for byte.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_aicar1.cpp
//        /Fo<dir>\ /Fe<dir>\world_aicar1.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_aicar1.exe [iterations] [seed] [function]
//
// out\race_v10.exe is loaded at 0x400000 the way test/fuzz.cpp does it. A real AICar (its vtable 0x4dbc28, 4224
// bytes) is filled from random state: the Car fields the AI reads (frame, velocity, wheels' ground / water /
// broken bytes, race state, steering), and every AICar field -- timers around `now`, the target and centre-line
// points, the contact and schedule state, the message list. Its racing line is a small real one: a closed loop of
// 4..40 ILSegs (the ConstIdealLine vtable, head, count, a bead segment -- sometimes NULL), the per-segment
// SegmentInfo notes, a proximity table, a skill record, the track-info table, the RaceDeity lap clocks and a
// deity whose vtable is three logging stubs. Everything the functions call runs as the original code in the image
// -- this file's own callees (count_wheels, _MSG, get_ilpos, teleport_to_track, the localifies,
// get_mpath_target, ...), IdealLine::get_actual_bead_position, Car's control setters, AIGetTrackInfo, the proxer
// constructor -- except what reaches outside, patched with a jump to a logging stub:
//   PhysicsGetTime, AIGetTrackNumber, AIGetSkill, WorldGetCarEntry, stricmp, LogReport, LogPanic,
//   PhysReplayAddEvent / Install / UninstallEventHandler, AIRegisterAICar / AIUnregisterCar, AIGetLine,
//   MemAlloc (a deterministic arena) / operator delete, GetDriverData / ReleaseDriverData, aicar_record_notes,
//   TerrainGetHeight(x,z), GetGameState, IdealLine::update_car_info / reset_bead_position (scripted beads, NULL
//   among them: the stranded-teleport crash) / get_rabbit_position (a scripted rabbit), AICar::init_rt_lat (the
//   second half's; it sets center_lat), LocalCar's constructor and destructor, and Car's Reset, UpdateCommon,
//   GetMessage, MakeReplayPacket, UpdateReplay, ResolveExternalImpulse and ApplyExternalForce (these nudge the
//   state their callers read afterwards, so a read hoisted above the call shows).
// Each world picks a function; the original runs, the state and the logs are kept, the starting state is
// restored, the rewrite runs, and every block, the return value and the logs are compared. The bytes the
// original changed must also lie inside the rewrite's footprint. A share of the worlds ("wild") has floats
// replaced by extreme values.
//
// The fixes (port.h: VP_FIX): the plain build defines VP_FAITHFUL and checks the rewrites against the originals as
// above. Built with /DFIX_TESTS it compiles the fixed rewrites: every world must still equal the original, except
// check_for_too_fast on a lost (NULL) bead, where the original faults and the fixed one must run (and every byte it
// changes must be in its footprint); then the fixes on their own: check_for_too_fast on a NULL bead that
// reset_bead_position finds (the same result as the original's on that bead) or doesn't (not too fast), and
// stuff_event's long texts (cut to 31 characters).
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

#ifndef FIX_TESTS
#define VP_FAITHFUL                                 // the original's behaviour, bit for bit (the fixes: /DFIX_TESTS)
#endif
#include "../hook/port.h"
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) {
    uint32_t vt = *(uint32_t*)obj;
    if (vt == 0x004dbc28) add(obj, 4224, what);
    else printf("footprint: %s at %p has an unknown class (vtable %08x)\n", what, obj, vt);
}

#include "../hook/phys_aicar1.cpp"

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

// ---- the world ----------------------------------------------------------------------------------------------
enum { CAR_BUF = 4224, NSEG_MAX = 40, SKILL_BYTES = 0x200, ENTRY_BYTES = 0x40, RDEITY_BYTES = 0x800, HEAP_BYTES = 0x4000,
       PKT = 0x48, MSGBUF = 0x400, NPROXER = 16 };
static uint8_t* g_carbuf;
static AICar* car() { return (AICar*)g_carbuf; }
static ILSeg g_segs[NSEG_MAX];
static uint8_t g_line[64];                       // a ConstIdealLine (60 bytes)
static SegmentInfo g_seginfo[NSEG_MAX];
static ProxerInfo g_proxer[NPROXER];
static uint8_t g_skill[SKILL_BYTES];
static uint8_t g_entry[ENTRY_BYTES];
static uint8_t g_rdeity[RDEITY_BYTES];
static uint8_t g_heap[HEAP_BYTES];
static uint32_t g_heap_used;
static uint8_t g_p0[PKT], g_p1[PKT], g_pkt[PKT];
static uint8_t g_msgbuf[MSGBUF];
static uint8_t g_notes[NSEG_MAX * 8];
static uint8_t g_event[0x2c];
static void* g_deity_vt[32];
static struct { void** vt; uint8_t rest[60]; } g_deity_obj;
static uint8_t* const g_trackinfo = (uint8_t*)0x004ec6c0;   // AIGetTrackInfo's table, 16 x 20 bytes
enum { TRACKINFO_BYTES = 16 * 20 };
static int g_nsegs;
static IdealLine* line() { return (IdealLine*)g_line; }
// the world's call's pointer outputs
static Point2D g_pt2;
static ILinePos g_ilpos;

// ---- stubs: scripts and the log ------------------------------------------------------------------------------
struct Script {
    float time; int track; int line_mode;       // AIGetLine: 0 the line, 1 a line without a head
    int alloc_fail;                              // bit per MemAlloc call: NULL
    uint8_t have_notes; float terrain_h;
    int bead_after_reset;                        // reset_bead_position's bead: -1 NULL, else a segment
    float bead_t_after;
    int bead_after_update;                       // update_car_info's: -2 unchanged, -1 NULL, else a segment
    Point2D rabbit_pos, rabbit_vel;
    int game_state; uint8_t teleport_ok; P3 teleport_move;
    int lap, laps;
    float rt_center_lat;                         // init_rt_lat's center_lat
    float resolve_now_nudge;                     // Car::ResolveExternalImpulse moves `now` (read after it)
    float force_rot_nudge;                       // Car::ApplyExternalForce turns the frame (read after it)
    int stricmp_result;
};
struct Log { int n; uint32_t e[256]; };
static Script g_script;
static Log g_log;
static void log_put(uint32_t v) { if (g_log.n < 256) g_log.e[g_log.n] = v; g_log.n++; }
static void log_bytes(const void* p, int n) { for (int i = 0; i + 4 <= n; i += 4) { uint32_t v; memcpy(&v, (const uint8_t*)p + i, 4); log_put(v); } }
static int g_alloc_i;

static float __cdecl stub_time() { log_put(0x71000000u); return g_script.time; }
static int __cdecl stub_tracknum() { log_put(0x7a000000u); return g_script.track; }
static uint8_t* __cdecl stub_getskill(int ci) { log_put(0x5c110000u); log_put(ci); return g_skill; }
static uint8_t* __cdecl stub_carentry(int ci) { log_put(0xe0000000u); log_put(ci); return g_entry; }
static int __cdecl stub_stricmp(const char* a, const char* b) { log_put(0x57000000u); log_put((uint32_t)a); log_put((uint32_t)b); return g_script.stricmp_result; }
static void __cdecl stub_logreport(const char* fmt, uint32_t a, uint32_t b, uint32_t c) { log_put(0x10900000u); log_put((uint32_t)fmt); log_put(a); log_put(b); log_put(c); }
static void __cdecl stub_logpanic(const char* fmt) { log_put(0x9a41c000u); log_put((uint32_t)fmt); }
static void __cdecl stub_addevent(int type, const void* data, int size) {
    log_put(0xadde0000u); log_put(type); log_put(size);
    if (data && size > 0 && size <= 0x100) log_bytes(data, size);
}
static void __cdecl stub_install(int type, void* h) { log_put(0x1257a11u); log_put(type); log_put((uint32_t)h); }
static void __cdecl stub_uninstall(void* h) { log_put(0x0a157a11u); log_put((uint32_t)h); }
static void __cdecl stub_register(void* c, int ci) { log_put(0x4e900000u); log_put((uint32_t)c); log_put(ci); }
static void __cdecl stub_unregister(void* c, int ci) { log_put(0x04e90000u); log_put((uint32_t)c); log_put(ci); }
static IdealLine* __cdecl stub_getline(int ci) {
    log_put(0x11e00000u); log_put(ci);
    if (g_script.line_mode == 1) line()->head = 0;
    return line();
}
static void* __cdecl stub_memalloc(int size) {
    log_put(0xa110c000u); log_put(size);
    const int i = g_alloc_i++;
    if (g_script.alloc_fail & (1 << (i & 7))) return 0;
    uint32_t n = ((uint32_t)size + 15u) & ~15u;
    if (size < 0 || g_heap_used + n > HEAP_BYTES) return 0;
    void* p = g_heap + g_heap_used;
    g_heap_used += n;
    return p;
}
static void __cdecl stub_delete(void* p) { log_put(0xde1e7e00u); log_put((uint32_t)p); }
static const uint8_t* __cdecl stub_driverdata(int ci) { log_put(0xd7d00000u); log_put(ci); return g_script.have_notes ? g_notes : 0; }
static void __cdecl stub_release(const uint8_t* p) { log_put(0x7e1ea5e0u); log_put((uint32_t)p); }
static void __cdecl stub_record_notes(const void* p, int n) {
    log_put(0x7ec07d00u); log_put((uint32_t)p); log_put(n);
    if (p && n > 0 && n <= NSEG_MAX * 8) log_bytes(p, n);
}
static float __cdecl stub_terrain_xz(uint32_t x, uint32_t z) { log_put(0x7e450000u); log_put(x); log_put(z); return g_script.terrain_h; }
static int __cdecl stub_gamestate() { log_put(0x6a3e0000u); return g_script.game_state; }
static ILSeg* seg_of(int i) { return i < 0 ? 0 : &g_segs[i % g_nsegs]; }
static void __fastcall stub_update_car_info(IdealLine* l, int, const Point2D* pos, const Point2D* dir) {
    log_put(0xca710000u); log_put((uint32_t)l);
    log_bytes(pos, 8); log_bytes(dir, 8);
    memcpy((uint8_t*)l + 0x18, pos, 8);
    memcpy((uint8_t*)l + 0x20, dir, 8);
    if (g_script.bead_after_update != -2) l->bead_seg = seg_of(g_script.bead_after_update);
}
static void __fastcall stub_reset_bead(IdealLine* l, int) {
    log_put(0xbead0000u); log_put((uint32_t)l);
    l->bead_seg = seg_of(g_script.bead_after_reset);
    l->bead_t = g_script.bead_t_after;
}
static void __fastcall stub_rabbit(IdealLine* l, int, Point2D* pos, Point2D* vel, uint32_t dist, ILinePos* out) {
    log_put(0x4abb1700u); log_put((uint32_t)l); log_put(dist);
    log_put((uint32_t)pos); log_put((uint32_t)vel);
    log_bytes(out, 8);                              // the caller's {0, 0}
    *pos = g_script.rabbit_pos;
    *vel = g_script.rabbit_vel;
    out->seg = seg_of(3);
    out->t = 0.25f;
    g_line[0x14] = 1;
}
static uint8_t __fastcall stub_deity_teleport(void* d, int, int ci, AICar* c) {
    log_put(0xde170001u); log_put((uint32_t)d); log_put(ci); log_put((uint32_t)c);
    if (g_script.teleport_ok) {
        c->frame.pos.x += g_script.teleport_move.x; c->frame.pos.y += g_script.teleport_move.y; c->frame.pos.z += g_script.teleport_move.z;
    }
    return g_script.teleport_ok;
}
static int __fastcall stub_deity_lap(void* d, int, int ci) { log_put(0xde170048u); log_put((uint32_t)d); log_put(ci); return g_script.lap; }
static int __fastcall stub_deity_laps(void* d, int) { log_put(0xde170020u); log_put((uint32_t)d); return g_script.laps; }
static void __fastcall stub_deity_bad(void* d, int) { log_put(0xde17bad0u); log_put((uint32_t)d); }
static void __fastcall stub_init_rt_lat(AICar* c, int) { log_put(0x2a7000u); log_put((uint32_t)c); c->center_lat = g_script.rt_center_lat; }
static void* __fastcall stub_localcar_ctor(AICar* c, int, void* data, void* p) {
    log_put(0x10ca1c70u); log_put((uint32_t)c); log_put((uint32_t)data); log_put((uint32_t)p);
    c->vtable = (void**)0x004dc5f0;
    c->car_index = (int32_t)(uintptr_t)data & 7;
    return c;
}
static void __fastcall stub_localcar_dtor(AICar* c, int) { log_put(0x10cad700u); log_put((uint32_t)c); }
static void __fastcall stub_car_reset(AICar* c, int) { log_put(0xca7e5e70u); log_put((uint32_t)c); }
static void __fastcall stub_car_common(AICar* c, int) { log_put(0xca7c0000u); log_put((uint32_t)c); }
static void __fastcall stub_car_getmessage(AICar* c, int, uint8_t* buf) { log_put(0xca7e5500u); log_put((uint32_t)c); log_put((uint32_t)buf); }
static void __fastcall stub_car_makepacket(AICar* c, int, uint8_t* p) {
    log_put(0xca7ac000u); log_put((uint32_t)c); log_put((uint32_t)p);
    for (int i = 0; i < PKT; i++) p[i] = (uint8_t)(i * 7 + 1);
}
static void __fastcall stub_car_updatereplay(AICar* c, int, const uint8_t* p0, const uint8_t* p1, uint32_t t) {
    log_put(0xca7e9000u); log_put((uint32_t)c); log_put((uint32_t)p0); log_put((uint32_t)p1); log_put(t);
    c->num_msgs = 3;                                // AICar's clears it after
    c->target_world.x = 1234.5f;
}
static void __fastcall stub_car_resolve(AICar* c, int, const P3* j, const P3* pt, int type) {
    log_put(0xca7e5010u); log_put((uint32_t)c); log_bytes(j, 12); log_bytes(pt, 12); log_put(type);
    c->now = c->now + g_script.resolve_now_nudge;
}
static void __fastcall stub_car_force(AICar* c, int, const P3* f, const P3* pt, int type) {
    log_put(0xca7f0000u); log_put((uint32_t)c); log_bytes(f, 12); log_bytes(pt, 12); log_put(type);
    float* m = c->frame.rot.m;
    m[0] += g_script.force_rot_nudge; m[2] -= g_script.force_rot_nudge; m[8] += g_script.force_rot_nudge;
}

// ---- building a world ------------------------------------------------------------------------------------------
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
static float speed_pick() {
    static const float edges[] = {0.4444f, 0.44444447f, 2.2222f, 2.2222223f, 8.888f, 8.8888893f, 22.222f, 22.222223f, 33.333f, 33.333336f};
    int k = rnd() % 10;
    if (k < 2) return edges[rnd() % 10];
    if (k < 4) return range(0.0f, 3.0f);
    if (k < 6) return range(3.0f, 25.0f);
    return range(20.0f, 80.0f);
}

static void build_line() {
    g_nsegs = 4 + rnd() % (NSEG_MAX - 3);
    float ct = 0.0f, cd = 0.0f;
    for (int i = 0; i < NSEG_MAX; i++) {
        ILSeg& s = g_segs[i];
        uint8_t* b = (uint8_t*)&s;
        for (int k = 0; k < (int)sizeof s; k += 4) { float f = range(-50.0f, 50.0f); memcpy(b + k, &f, 4); }
        s.next = &g_segs[(i + 1) % g_nsegs];
        s.target_speed = range(10.0f, 80.0f);
        s.curvature = chance(20) ? range(-0.02f, 0.1f) : range(0.0f, 0.03f);
        s.step = range(2.0f, 12.0f);
        s.cum_distance = cd;
        s.index = (int16_t)i;
        s.cum_time = ct;
        cd += s.step;
        ct += s.step / s.target_speed;
    }
    memset(g_line, 0, sizeof g_line);
    for (int k = 0; k < 60; k += 4) { float f = range(-10.0f, 10.0f); memcpy(g_line + k, &f, 4); }
    IdealLine* l = line();
    l->vtable = (void**)0x004db6e8;
    l->head = &g_segs[0];
    l->num_segs = g_nsegs;
    l->bead_seg = chance(8) ? 0 : &g_segs[rnd() % g_nsegs];
    l->bead_t = chance(10) ? (chance(50) ? 0.0f : 1.0f) : uni();
    for (int i = 0; i < NSEG_MAX; i++) {
        SegmentInfo& si = g_seginfo[i];
        si.notes.hit_wall = (uint8_t)(rnd() & 1); si.notes.off_road_a = (uint8_t)(rnd() & 1); si.notes.off_road_b = (uint8_t)(rnd() & 1);
        si.notes._3 = (uint8_t)rnd();
        si.notes.speed_note = chance(40) ? 0.0f : range(-8.0f, 8.0f);
        si.lateral_note = chance(40) ? (chance(50) ? 0.0f : -0.0f) : range(-3.0f, 3.0f);
        si.locked = (uint8_t)(rnd() & 1);
        si._0d[0] = (uint8_t)rnd(); si._0d[1] = (uint8_t)rnd(); si._0d[2] = (uint8_t)rnd();
        si.lookahead = range(0.3f, 0.7f);
        si.seg = &g_segs[i % g_nsegs];
    }
    for (int i = 0; i < NSEG_MAX * 8; i += 4) { float f = range(-5.0f, 5.0f); memcpy(g_notes + i, &f, 4); }
}

static void randomize_car() {
    AICar* c = car();
    uint8_t* b = g_carbuf;
    for (int i = 0; i < CAR_BUF; i++) b[i] = (uint8_t)rnd();
    c->vtable = VT_AICar;
    *(uint32_t*)(b + 8) = 0x20 + 4 * (rnd() % 16);           // PhobRoot +8: the message offset
    rotation(&c->frame.rot, !chance(20));
    c->frame.pos.x = range(-2000.0f, 2000.0f); c->frame.pos.y = range(-50.0f, 300.0f); c->frame.pos.z = range(-2000.0f, 2000.0f);
    const float sp = speed_pick();
    const float* m = c->frame.rot.m;
    const float dir = chance(80) ? 1.0f : -1.0f;
    c->velocity.x = m[6] * sp * dir + range(-1.0f, 1.0f); c->velocity.y = range(-2.0f, 2.0f); c->velocity.z = m[8] * sp * dir + range(-1.0f, 1.0f);
    c->body_volume = 0;
    memset(c->live_models, 0, sizeof c->live_models);
    c->realism = rnd() % 3;
    c->yaw_control = rnd() % 3;
    static const int states[] = {0, 2, 2, 2, 3};
    c->race_state = states[rnd() % 5];
    c->steering = chance(20) ? 0.0f : range(-1.0f, 1.0f);
    c->car_index = rnd() % 8;
    c->auto_clutch = (uint8_t)(rnd() & 1);
    for (int i = 0; i < 4; i++) {
        Wheel& w = c->wheels[i];
        w.broken = chance(8);
        w.on_ground = chance(80) ? 1 : (chance(80) ? 0 : (uint8_t)rnd());
        w.surface = chance(10) ? 14 : (int32_t)(rnd() % 24);
        w.steer_input = range(-0.5f, 0.5f);
        w.tire_sound = 0;
    }
    c->max_steer_angle = range(0.3f, 0.7f);
    c->braking = chance(40) ? 0.0f : uni();
    c->lat_g = range(-2.0f, 2.0f);
    memset(c->sounds, 0, sizeof c->sounds);
    // the AI's own
    const float now = range(0.0f, 400.0f);
    c->drive_fn = (void*)(uintptr_t)0x00431ce0;
    c->interact_fn = (void*)(uintptr_t)0x004312a0;
    c->slam_steer = chance(30) ? 0.0f : range(-0.05f, 0.05f);
    c->contact_valid = chance(60);
    c->contact_multiple = chance(30);
    {
        int k = rnd() % 6;
        c->contact_time = k == 0 ? bitsf(0x3e99999a + (rnd() % 3) - 1) : k == 1 ? bitsf(0x40800000 + (rnd() % 3) - 1) : k == 2 ? range(-1.0f, 6.0f) : range(0.3f, 4.0f);
    }
    c->contact_line_lat = range(-10.0f, 10.0f);
    c->line_lat = range(-10.0f, 10.0f);
    if (chance(20)) c->contact_line_lat = c->line_lat + range(-7.0f, 7.0f);
    c->pass_offset = chance(40) ? 0.0f : range(-6.7f, 6.7f);
    c->contact_center_lat = range(-12.0f, 12.0f);
    c->now = now;
    c->speed = speed_pick();
    c->track_half_width = range(5.0f, 15.0f);
    c->center_lat = chance(10) ? (chance(50) ? 0.0f : -0.0f) : range(-12.0f, 12.0f);
    c->start_lat = range(-5.0f, 5.0f);
    c->finished = chance(15);
    c->lap = chance(25) ? 0 : 1 + rnd() % 5;
    c->progress_seg = chance(30) ? line()->bead_seg : &g_segs[rnd() % g_nsegs];
    c->steer_damping = chance(30) ? bitsf(0x3f333333 + (rnd() % 3) - 1) : range(0.0f, 0.8f);
    c->steer = chance(10) ? (chance(50) ? 1.0f : -1.0f) : range(-1.0f, 1.0f);
    c->tc_always = chance(30);
    c->skill = g_skill;
    c->target_world.x = range(-2000.0f, 2000.0f); c->target_world.z = range(-2000.0f, 2000.0f);
    c->target_pos.x = c->frame.pos.x + range(-60.0f, 60.0f); c->target_pos.z = c->frame.pos.z + range(-60.0f, 60.0f);
    c->target_vel.x = range(-60.0f, 60.0f); c->target_vel.z = range(-60.0f, 60.0f);
    if (chance(5)) { c->target_vel.x = 0.0f; c->target_vel.z = chance(50) ? 0.0f : 1e-8f; }
    c->target_speed = range(0.0f, 80.0f);
    c->center_pos.x = c->frame.pos.x + range(-10.0f, 10.0f); c->center_pos.z = c->frame.pos.z + range(-10.0f, 10.0f);
    c->center_recover_vel.x = range(-26.7f, 26.7f); c->center_recover_vel.z = range(-26.7f, 26.7f);
    c->center_tan.x = range(-1.0f, 1.0f); c->center_tan.z = range(-1.0f, 1.0f);
    c->edge_offset.x = range(-8.0f, 8.0f); c->edge_offset.z = range(-8.0f, 8.0f);
    c->center_right.x = c->center_tan.z; c->center_right.z = -c->center_tan.x;
    c->progress_time = now - (chance(50) ? range(0.0f, 4.0f) : range(4.0f, 20.0f));
    c->finish_time = chance(60) ? (chance(50) ? 0.0f : -0.0f) : now - range(0.0f, 30.0f);
    c->crash_time = chance(30) ? -1000.0f : now - range(0.0f, 20.0f);
    c->bump_time = chance(30) ? -1000.0f : now - range(0.0f, 20.0f);
    c->fouroff_time = chance(40) ? -1000.0f : chance(10) ? now - 12.0f : now - range(0.0f, 16.0f);
    c->race_start_time = chance(20) ? now - 10.0f : now - range(0.0f, 30.0f);
    c->freeze_time = chance(50) ? -1000.0f : now - range(0.0f, 10.0f);
    c->wall_heat = range(0.0f, 60.0f);
    c->recover_side = chance(50) ? 1.0f : -1.0f;
    c->offground_cut = chance(40) ? 0.0f : range(0.0f, 0.4f);
    c->line_update_time = now - range(0.0f, 1.0f);
    c->line = chance(8) ? 0 : line();
    c->num_msgs = chance(10) ? (int32_t)(rnd() % 14) - 2 : (int32_t)(rnd() % 11);
    for (int i = 0; i < 10; i++) {
        const int k = rnd() % 4;
        c->msgs[i] = k == 0 ? (const char*)(uintptr_t)(0x004ed000 + (rnd() & 0xfff)) : ((const char* const*)0x004ed408)[rnd() % 24];
    }
    c->min_brake = 0.0f;
    c->too_fast = chance(40) ? 0.0f : chance(10) ? range(-0.5f, 1.5f) : uni();
    c->lap_target_time = range(30.0f, 120.0f);
    c->base_lap_time = chance(30) ? range(-10.0f, -1.0f) : range(30.0f, 120.0f);
    c->lap_time_spread = range(0.0f, 5.0f);
    c->pace = range(0.8f, 1.3f);
    c->schedule_error = range(-5.0f, 5.0f);
    c->lap_clock = (float*)(g_rdeity + 0x78 + 0x74 * c->car_index);
    c->throttle_cap = chance(40) ? 1.0f : range(0.5f, 1.0f);
    c->lap_time_offset = range(0.0f, 5.0f);
    c->line_lap_time = range(30.0f, 120.0f);
    c->proxer_info = g_proxer;
    c->num_proxer_info = chance(10) ? 0 : 1 + rnd() % NPROXER;
    c->learn_hiatus = (int32_t)(rnd() % 20) - 3;
    c->seginfo = g_seginfo;
    c->num_segs = g_nsegs;
    c->seg_index = rnd() % g_nsegs;
    c->next_seg_index = (c->seg_index + 1) % g_nsegs;
    c->seg_t = uni();
    c->seg_changed = (uint8_t)(rnd() & 1);
    c->seg_valid = chance(80);
    c->wall_hit_lat = range(-10.0f, 10.0f);
    c->last_learn_lap = rnd() % 5;
    c->latslam_rel_speed = range(-5.0f, 5.0f);
    c->launch_time = range(1.0f, 6.0f);
    {
        int k = rnd() % 5;
        c->teleport_armed_time = k == 0 ? -100.0f : k == 1 ? now - range(0.0f, 2.0f) : k == 2 ? now - range(2.0f, 5.0f) : k == 3 ? now - bitsf(chance(50) ? 0x40000000 : 0x40a00000) : now - range(5.0f, 50.0f);
    }
}

static void randomize_world() {
    build_line();
    for (int i = 0; i < SKILL_BYTES; i += 4) { float f = range(0.0f, 2.0f); memcpy(g_skill + i, &f, 4); }
    *(float*)(g_skill + 0) = range(0.0f, 5.0f);
    *(float*)(g_skill + 8) = chance(20) ? bitsf(0x3f4ccccd) : range(0.0f, 1.2f);
    *(float*)(g_skill + 0x1c) = range(0.5f, 1.2f);
    *(float*)(g_skill + 0x24) = range(0.8f, 1.1f);
    *(float*)(g_skill + 0x28) = range(0.3f, 2.0f);
    *(float*)(g_skill + 0x3c) = range(0.3f, 3.0f);
    *(float*)(g_skill + 0x40) = range(0.3f, 3.0f);
    for (int t = 0; t < 16; t++) {
        *(float*)(g_skill + 0x98 + 0x18 * t) = chance(30) ? range(-10.0f, -1.0f) : range(30.0f, 120.0f);
        *(float*)(g_skill + 0xa0 + 0x18 * t) = range(0.5f, 1.5f);
        *(float*)(g_skill + 0xa4 + 0x18 * t) = range(0.0f, 5.0f);
    }
    memcpy(g_skill + 0x70, "viper\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 32);
    for (int i = 0; i < ENTRY_BYTES; i++) g_entry[i] = (uint8_t)rnd();
    memcpy(g_entry + 0x11, chance(80) ? "viper" : "cobra", 6);
    for (int i = 0; i < RDEITY_BYTES; i += 4) { float f = range(0.0f, 150.0f); memcpy(g_rdeity + i, &f, 4); }
    for (int i = 0; i < NPROXER; i++) { g_proxer[i].level = range(0.0f, 2.0f); g_proxer[i]._4 = rnd(); }
    for (int i = 0; i < 16; i++) {
        uint8_t* ti = g_trackinfo + 20 * i;
        *(float*)(ti + 0) = range(0.0f, 3.0f);
        *(float*)(ti + 4) = range(0.0f, 1.0f);
        ti[8] = (uint8_t)(rnd() & 1); ti[9] = (uint8_t)rnd(); ti[10] = (uint8_t)rnd(); ti[11] = (uint8_t)rnd();
        *(int32_t*)(ti + 12) = rnd() % 4;
        *(float*)(ti + 16) = range(0.0f, 5.0f);
    }
    for (int i = 0; i < PKT; i++) { g_p0[i] = (uint8_t)rnd(); g_p1[i] = (uint8_t)rnd(); g_pkt[i] = (uint8_t)rnd(); }
    if (chance(50)) { uint32_t mk = rnd() & (chance(50) ? 0xffffffffu : 0x00ffffffu); memcpy(g_p0 + 0x44, &mk, 4); }
    for (int i = 0; i < MSGBUF; i++) g_msgbuf[i] = (uint8_t)rnd();
    for (int i = 0; i < HEAP_BYTES; i++) g_heap[i] = 0xcd;
    randomize_car();
    // the statics
    *(int32_t*)0x00521c74 = chance(30) ? 0 : chance(30) ? 1 : (int32_t)(rnd() % 8);   // AICar::instance_count
    *(int32_t*)0x004eb66c = chance(10) ? 0 : 1 + rnd() % 8;                            // AICarCount
    *(uint8_t*)0x004eb648 = chance(30);                                                // ai_learn_mode
    *(uint8_t*)0x005218e8 = chance(20);                                                // PhysReplayPlayMode
    // the script
    AICar* c = car();
    g_script.time = chance(50) ? c->now + range(0.0f, 0.1f) : range(0.0f, 400.0f);
    g_script.track = rnd() % 16;
    g_script.line_mode = chance(10) ? 1 : 0;
    g_script.alloc_fail = chance(10) ? (int)(rnd() & 0xff) : 0;
    g_script.have_notes = chance(60);
    g_script.terrain_h = c->frame.pos.y + range(-2.0f, 2.0f);
    g_script.bead_after_reset = chance(15) ? -1 : (int)(rnd() % g_nsegs);
    g_script.bead_t_after = uni();
    g_script.bead_after_update = chance(60) ? -2 : chance(20) ? -1 : (int)(rnd() % g_nsegs);
    g_script.rabbit_pos.x = c->frame.pos.x + range(-80.0f, 80.0f); g_script.rabbit_pos.z = c->frame.pos.z + range(-80.0f, 80.0f);
    g_script.rabbit_vel.x = range(-60.0f, 60.0f); g_script.rabbit_vel.z = range(-60.0f, 60.0f);
    g_script.game_state = chance(70) ? 1 : (int)(rnd() % 4);
    g_script.teleport_ok = chance(60);
    g_script.teleport_move.x = range(-20.0f, 20.0f); g_script.teleport_move.y = range(-2.0f, 2.0f); g_script.teleport_move.z = range(-20.0f, 20.0f);
    g_script.lap = rnd() % 6;
    g_script.laps = chance(5) ? 0 : 1 + rnd() % 8;
    g_script.rt_center_lat = chance(10) ? 0.0f : range(-10.0f, 10.0f);
    g_script.resolve_now_nudge = chance(50) ? 0.0f : range(0.01f, 1.0f);
    g_script.force_rot_nudge = chance(50) ? 0.0f : range(0.001f, 0.1f);
    g_script.stricmp_result = chance(80) ? 0 : 1;
    if (chance(10)) {                                // a target on top of the car: the direction normalisations' skip
        g_script.rabbit_vel.x = chance(50) ? 0.0f : 1e-9f; g_script.rabbit_vel.z = 0.0f;
        g_script.rabbit_pos.x = c->frame.pos.x; g_script.rabbit_pos.z = c->frame.pos.z; g_script.terrain_h = c->frame.pos.y;
        c->pass_offset = 0.0f; c->contact_valid = 0; c->seg_valid = 0; c->fouroff_time = -1000.0f; c->race_start_time = -1000.0f;
    }
    if (chance(50) && c->line && line()->bead_seg) {  // on schedule, within seconds: the rubber band acts
        const ILSeg* sg = line()->bead_seg;
        const float sched = (float)(((D(sg->next->cum_time) - sg->cum_time) * line()->bead_t + sg->cum_time) * c->pace);
        *c->lap_clock = sched + (chance(70) ? range(-0.6f, 0.2f) : range(-4.0f, 4.0f));
        c->lap_target_time = *c->lap_clock + (chance(90) ? range(0.5f, 40.0f) : range(-5.0f, 0.5f));
        c->schedule_error = (float)(D(*c->lap_clock) - sched) + range(-0.3f, 0.3f);
        if (chance(50)) { c->speed = range(22.3f, 80.0f); c->finished = 0; if (!c->lap) c->lap = 1; }
    }
}

enum Kind : int;
struct Args {
    float f; uint32_t u; int32_t a, b, type; uint8_t on; P3 j, pt; const SegmentInfo* s; void* pred; int pred_k;
    char text[40]; int32_t ev_kind; int32_t ev_value; void* ctor_data; void* ctor_p;
};
static Args g_args;

static void randomize_args() {
    AICar* c = car();
    g_args.f = chance(10) ? wildf() : range(-1.5f, 1.5f);
    g_args.u = fbits(chance(10) ? wildf() : chance(10) ? bitsf(0x80000000u + (rnd() % 3)) : range(-0.5f, 1.5f));
    g_args.a = (int32_t)(rnd() % (3 * g_nsegs)) - g_nsegs;
    g_args.b = (int32_t)(rnd() % (3 * g_nsegs)) - g_nsegs;
    static const int types[] = {0, 0x6d, 0x6d, 0x6e, 0x6e, 1, 5};
    g_args.type = types[rnd() % 7];
    g_args.on = chance(70) ? 1 : (chance(50) ? 0 : (uint8_t)rnd());
    const float js = chance(50) ? range(0.0f, 2000.0f) : range(1000.0f, 20000.0f);
    g_args.j.x = range(-js, js); g_args.j.y = range(-js, js); g_args.j.z = range(-js, js);
    g_args.pt = c->frame.pos;
    g_args.pt.x += range(-2.0f, 2.0f);
    g_args.s = &g_seginfo[rnd() % g_nsegs];
    g_args.pred_k = rnd() % 3;
    g_args.pred = (void*)(uintptr_t)(g_args.pred_k == 0 ? 0x0042f160 : g_args.pred_k == 1 ? 0x0042f180 : 0x0042f920);
    for (int i = 0; i < 31; i++) g_args.text[i] = (char)('a' + rnd() % 26);
    g_args.text[31] = 0;
    g_args.ev_kind = chance(85) ? (int32_t)(rnd() & 1) : (int32_t)rnd();
    g_args.ev_value = (int32_t)rnd();
    g_args.ctor_data = (void*)(uintptr_t)(0x100 + (rnd() % 8));
    g_args.ctor_p = (void*)(uintptr_t)rnd();
    g_pt2.x = c->frame.pos.x + range(-100.0f, 100.0f);
    g_pt2.z = c->frame.pos.z + range(-100.0f, 100.0f);
    if (chance(20)) { g_pt2.x = range(-60.0f, 60.0f); g_pt2.z = range(-60.0f, 60.0f); }
    memset(&g_ilpos, 0xcc, sizeof g_ilpos);
    memcpy(g_event, &g_args.ev_kind, 4);
    for (int i = 4; i < 0x2c; i++) g_event[i] = (uint8_t)rnd();
}

// some floats replaced with extreme values
static const uint32_t k_ai_floats[] = {0x38, 0x3c, 0x40, 0x48, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x64, 0x234, 0x238, 0x23c, 0x500, 0xdcc, 0xde4, 0xea4,
    0xef0, 0xef8, 0xefc, 0xf00, 0xf04, 0xf0c, 0xf10, 0xf14, 0xf18, 0xf24, 0xf3c, 0xf48, 0xf4c, 0xf64, 0xf68, 0xf6c, 0xf70, 0xf84, 0xf88,
    0xf8c, 0xf90, 0xf94, 0xfa0, 0xfa4, 0xfb0, 0xfb4, 0xfb8, 0xfbc, 0xfc8, 0xfcc, 0xfd4, 0xfdc, 0xfe0, 0x1020, 0x1024, 0x1028, 0x1030,
    0x1034, 0x103c, 0x1044, 0x1064, 0x1078, 0x107c};
static void wildify() {
    AICar* c = car();
    int n = 1 + rnd() % 4;
    for (int i = 0; i < n; i++) {
        uint32_t off = k_ai_floats[rnd() % (sizeof k_ai_floats / sizeof *k_ai_floats)];
        float f = wildf();
        memcpy(g_carbuf + off, &f, 4);
    }
    if (chance(30)) { static const uint32_t sk[] = {0, 8, 0x1c, 0x24, 0x28, 0x3c, 0x40}; float f = wildf(); memcpy(g_skill + sk[rnd() % 7], &f, 4); }
    if (chance(20)) { SegmentInfo& si = g_seginfo[c->seg_index]; float f = wildf(); memcpy(chance(50) ? (void*)&si.lookahead : chance(50) ? (void*)&si.lateral_note : (void*)&si.notes.speed_note, &f, 4); }
    if (chance(20)) { float f = wildf(); memcpy(chance(50) ? &g_script.rabbit_vel.x : &g_script.rabbit_pos.z, &f, 4); }
    if (chance(20)) { float f = wildf(); memcpy(c->lap_clock, &f, 4); }
    if (chance(15)) { ILSeg& s = g_segs[rnd() % g_nsegs]; float f = wildf(); memcpy(chance(50) ? &s.cum_time : chance(50) ? &s.curvature : &s.target_speed, &f, 4); }
    if (chance(15)) g_args.f = wildf();
    if (chance(15)) g_script.time = wildf();
    if (chance(20)) { float f = wildf(); memcpy(chance(50) ? &g_pt2.x : &g_pt2.z, &f, 4); }
    if (chance(10)) {                                // a difference past the float range (register vs stored)
        const float sg = chance(50) ? 1.0f : -1.0f;
        float* a = chance(50) ? &g_pt2.x : &g_pt2.z;
        *a = sg * range(2e38f, 3.4e38f);
        (a == &g_pt2.x ? c->frame.pos.x : c->frame.pos.z) = -sg * range(2e38f, 3.4e38f);
        if (chance(50)) { g_script.rabbit_pos.x = *a; g_script.rabbit_pos.z = *a; }
    }
}

// ---- running both --------------------------------------------------------------------------------------------
struct State {
    uint8_t car[CAR_BUF];
    ILSeg segs[NSEG_MAX];
    uint8_t line[64];
    SegmentInfo seginfo[NSEG_MAX];
    ProxerInfo proxer[NPROXER];
    uint8_t skill[SKILL_BYTES], entry[ENTRY_BYTES], rdeity[RDEITY_BYTES], heap[HEAP_BYTES];
    uint8_t p0[PKT], p1[PKT], pkt[PKT], msgbuf[MSGBUF], notes[NSEG_MAX * 8], event[0x2c], trackinfo[TRACKINFO_BYTES];
    Point2D pt2; ILinePos ilpos;
    uint8_t statics[16];
    uint64_t ret;
    Log log;
};
struct Block { void* live; size_t off, n; const char* name; };
#define BLK(field, live) {(void*)(live), offsetof(State, field), sizeof(((State*)0)->field), #field}
static Block g_blocks[] = {
    {0, offsetof(State, car), CAR_BUF, "car"}, BLK(segs, g_segs), BLK(line, g_line), BLK(seginfo, g_seginfo), BLK(proxer, g_proxer),
    BLK(skill, g_skill), BLK(entry, g_entry), BLK(rdeity, g_rdeity), BLK(heap, g_heap), BLK(p0, g_p0), BLK(p1, g_p1), BLK(pkt, g_pkt),
    BLK(msgbuf, g_msgbuf), BLK(notes, g_notes), BLK(event, g_event), BLK(trackinfo, 0x004ec6c0), BLK(pt2, &g_pt2), BLK(ilpos, &g_ilpos)};
static void save(State& s) {
    g_blocks[0].live = g_carbuf;
    for (const Block& b : g_blocks) memcpy((uint8_t*)&s + b.off, b.live, b.n);
    memcpy(s.statics + 0, (void*)0x00521c74, 4);
    memcpy(s.statics + 4, (void*)0x004eb66c, 4);
    s.statics[8] = *(uint8_t*)0x004eb648;
    s.statics[9] = *(uint8_t*)0x005218e8;
    memcpy(s.statics + 12, (void*)0x00520b10, 4);
}
static void load(const State& s) {
    for (const Block& b : g_blocks) memcpy(b.live, (const uint8_t*)&s + b.off, b.n);
    memcpy((void*)0x00521c74, s.statics + 0, 4);
    memcpy((void*)0x004eb66c, s.statics + 4, 4);
    *(uint8_t*)0x004eb648 = s.statics[8];
    *(uint8_t*)0x005218e8 = s.statics[9];
    memcpy((void*)0x00520b10, s.statics + 12, 4);
}

// the functions under test
enum Kind : int { K_REPL, K_STUFF, K_GETMSG, K_UPDREPLAY, K_COMMON, K_MAKEPKT, K_LOCVEC, K_LOCLOC, K_ILPOS, K_RESOLVE, K_FORCE,
    K_RESET, K_FAST, K_SLOW, K_CTOR, K_RESETP, K_DTOR, K_SUSPEND, K_FOUROFF, K_REL_II, K_REL_SI, K_THROTTLE, K_ONWATER, K_OFFGROUND,
    K_COUNT, K_STEER, K_BRAKE, K_YAW, K_TC, K_ABS, K_NOTES, K_MSG, K_TELEPORT, K_STRANDED, K_TOOFAST, K_DAMAGE, K_DAMAGED,
    K_INITSEG, K_LOOKAHEAD, K_RTINFO, K_LINEINFO, K_SEGINFO, K_MPATH, K_REAL, N_KINDS };
static const char* const kind_names[N_KINDS] = {"repl_handler", "stuff_event", "GetMessage", "UpdateReplay", "UpdateCommon",
    "MakeReplayPacket", "localify_vector", "localify_loc", "get_ilpos", "ResolveExternalImpulse", "ApplyExternalForce", "Reset",
    "fast_mode", "slow_mode", "AICar::AICar", "reset(private)", "~AICar", "suspend_adjustment", "fire_fouroff", "GetRelSegment(ii)",
    "GetRelSegment(si)", "AISetThrottle", "on_water", "off_ground", "count_wheels", "AISetSteering", "AISetBrake", "set_yaw_control",
    "set_traction_control", "set_abs", "Notes::clear", "_MSG", "teleport_to_track", "check_for_stranded", "check_for_too_fast",
    "check_for_damage", "damaged", "init_seginfo", "lookahead_from_curvature", "init_rtinfo", "update_line_info",
    "update_segment_info", "get_mpath_target", "get_real_target"};
static const int kind_weight[N_KINDS] = {1, 2, 1, 3, 1, 3, 3, 3, 2, 5, 5, 3, 1, 1, 3, 5, 3, 1, 2, 2, 2, 6, 1, 1, 2, 6, 6, 2, 2, 1,
    1, 3, 6, 8, 8, 5, 1, 4, 1, 4, 2, 5, 12, 10};

static uint64_t call_kind(Kind k, bool rw) {
    AICar* c = car();
    uint64_t r = 0;
#define O(T, A) ((T)(A))
    typedef void(__fastcall * V_t)(AICar*, int);
    switch (k) {
    case K_REPL: r = (uint32_t)(rw ? AICar_repl_handler(g_event, 0) : O(int(__cdecl*)(const void*, int), 0x42e5c0)(g_event, 0)); break;
    case K_STUFF: rw ? AICar_stuff_event(c, 0, g_args.ev_kind, g_args.ev_value, g_args.text)
                     : O(void(__fastcall*)(AICar*, int, int32_t, int32_t, const char*), 0x42e5f0)(c, 0, g_args.ev_kind, g_args.ev_value, g_args.text); break;
    case K_GETMSG: rw ? AICar_GetMessage(c, 0, g_msgbuf) : O(void(__fastcall*)(AICar*, int, uint8_t*), 0x42e660)(c, 0, g_msgbuf); break;
    case K_UPDREPLAY: rw ? AICar_UpdateReplay(c, 0, g_p0, g_p1, g_args.u)
                         : O(void(__fastcall*)(AICar*, int, const uint8_t*, const uint8_t*, uint32_t), 0x42e670)(c, 0, g_p0, g_p1, g_args.u); break;
    case K_COMMON: rw ? AICar_UpdateCommon(c, 0) : O(V_t, 0x42e6f0)(c, 0); break;
    case K_MAKEPKT: rw ? AICar_MakeReplayPacket(c, 0, g_pkt) : O(void(__fastcall*)(AICar*, int, uint8_t*), 0x42e710)(c, 0, g_pkt); break;
    case K_LOCVEC: rw ? AICar_localify_vector(c, 0, &g_pt2) : O(void(__fastcall*)(AICar*, int, Point2D*), 0x42e790)(c, 0, &g_pt2); break;
    case K_LOCLOC: rw ? AICar_localify_loc(c, 0, &g_pt2) : O(void(__fastcall*)(AICar*, int, Point2D*), 0x42e7d0)(c, 0, &g_pt2); break;
    case K_ILPOS: r = (uint32_t)(rw ? AICar_get_ilpos(c, 0, &g_ilpos) : O(ILinePos*(__fastcall*)(AICar*, int, ILinePos*), 0x42e860)(c, 0, &g_ilpos)); break;
    case K_RESOLVE: rw ? AICar_ResolveExternalImpulse(c, 0, &g_args.j, &g_args.pt, g_args.type)
                       : O(void(__fastcall*)(AICar*, int, const P3*, const P3*, int32_t), 0x42e8a0)(c, 0, &g_args.j, &g_args.pt, g_args.type); break;
    case K_FORCE: rw ? AICar_ApplyExternalForce(c, 0, &g_args.j, &g_args.pt, g_args.type)
                     : O(void(__fastcall*)(AICar*, int, const P3*, const P3*, int32_t), 0x42e970)(c, 0, &g_args.j, &g_args.pt, g_args.type); break;
    case K_RESET: rw ? AICar_Reset(c, 0) : O(V_t, 0x42ea60)(c, 0); break;
    case K_FAST: rw ? AICar_fast_mode(c, 0, g_args.text) : O(void(__fastcall*)(AICar*, int, const char*), 0x42ea80)(c, 0, g_args.text); break;
    case K_SLOW: rw ? AICar_slow_mode(c, 0, g_args.text) : O(void(__fastcall*)(AICar*, int, const char*), 0x42eaa0)(c, 0, g_args.text); break;
    case K_CTOR: r = (uint32_t)(rw ? AICar_ctor(c, 0, g_args.ctor_data, g_args.ctor_p)
                                  : O(AICar*(__fastcall*)(AICar*, int, void*, void*), 0x42eac0)(c, 0, g_args.ctor_data, g_args.ctor_p)); break;
    case K_RESETP: rw ? AICar_reset(c, 0) : O(V_t, 0x42ec80)(c, 0); break;
    case K_DTOR: rw ? AICar_dtor(c, 0) : O(V_t, 0x42eea0)(c, 0); break;
    case K_SUSPEND: rw ? AICar_suspend_adjustment(c, 0, g_args.text) : O(void(__fastcall*)(AICar*, int, const char*), 0x42ef80)(c, 0, g_args.text); break;
    case K_FOUROFF: rw ? AICar_fire_fouroff(c, 0) : O(V_t, 0x42efa0)(c, 0); break;
    case K_REL_II: r = (uint32_t)(rw ? AICar_GetRelSegment_ii(c, 0, g_args.a, g_args.b)
                                    : O(SegmentInfo*(__fastcall*)(AICar*, int, int32_t, int32_t), 0x42efd0)(c, 0, g_args.a, g_args.b)); break;
    case K_REL_SI: r = (uint32_t)(rw ? AICar_GetRelSegment_si(c, 0, g_args.s, g_args.a)
                                    : O(SegmentInfo*(__fastcall*)(AICar*, int, const SegmentInfo*, int32_t), 0x42f000)(c, 0, g_args.s, g_args.a)); break;
    case K_THROTTLE: rw ? AICar_AISetThrottle(c, 0, g_args.f) : O(void(__fastcall*)(AICar*, int, float), 0x42f030)(c, 0, g_args.f); break;
    case K_ONWATER: r = rw ? on_water(&c->wheels[g_args.a & 3]) : O(uint8_t(__cdecl*)(Wheel*), 0x42f160)(&c->wheels[g_args.a & 3]); break;
    case K_OFFGROUND: r = rw ? off_ground(&c->wheels[g_args.a & 3]) : O(uint8_t(__cdecl*)(Wheel*), 0x42f180)(&c->wheels[g_args.a & 3]); break;
    case K_DAMAGED: r = rw ? damaged(&c->wheels[g_args.a & 3]) : O(uint8_t(__cdecl*)(Wheel*), 0x42f920)(&c->wheels[g_args.a & 3]); break;
    case K_COUNT: r = (uint32_t)(rw ? count_wheels(g_args.pred, c->wheels) : O(int32_t(__cdecl*)(void*, Wheel*), 0x42f190)(g_args.pred, c->wheels)); break;
    case K_STEER: rw ? AICar_AISetSteering(c, 0, g_args.f) : O(void(__fastcall*)(AICar*, int, float), 0x42f1c0)(c, 0, g_args.f); break;
    case K_BRAKE: rw ? AICar_AISetBrake(c, 0, g_args.u) : O(void(__fastcall*)(AICar*, int, uint32_t), 0x42f310)(c, 0, g_args.u); break;
    case K_YAW: rw ? AICar_set_yaw_control(c, 0, g_args.on) : O(void(__fastcall*)(AICar*, int, uint8_t), 0x42f460)(c, 0, g_args.on); break;
    case K_TC: rw ? AICar_set_traction_control(c, 0, g_args.on) : O(void(__fastcall*)(AICar*, int, uint8_t), 0x42f4a0)(c, 0, g_args.on); break;
    case K_ABS: rw ? AICar_set_abs(c, 0, g_args.on) : O(void(__fastcall*)(AICar*, int, uint8_t), 0x42f4e0)(c, 0, g_args.on); break;
    case K_NOTES: rw ? Notes_clear((Notes*)g_args.s, 0) : O(void(__fastcall*)(Notes*, int), 0x42f500)((Notes*)g_args.s, 0); break;
    case K_MSG: {
        const char* m = chance(0) ? 0 : (g_args.a & 1) ? c->msgs[(uint32_t)g_args.b % 10] : ((const char* const*)0x004ed408)[(uint32_t)g_args.b % 24];
        rw ? AICar_MSG(c, 0, m) : O(void(__fastcall*)(AICar*, int, const char*), 0x42f510)(c, 0, m);
        break;
    }
    case K_TELEPORT: r = rw ? AICar_teleport_to_track(c, 0) : O(uint8_t(__fastcall*)(AICar*, int), 0x42f560)(c, 0); break;
    case K_STRANDED: rw ? AICar_check_for_stranded(c, 0) : O(V_t, 0x42f620)(c, 0); break;
    case K_TOOFAST: rw ? AICar_check_for_too_fast(c, 0) : O(V_t, 0x42f730)(c, 0); break;
    case K_DAMAGE: rw ? AICar_check_for_damage(c, 0) : O(V_t, 0x42f8b0)(c, 0); break;
    case K_INITSEG: rw ? AICar_init_seginfo(c, 0) : O(V_t, 0x42f930)(c, 0); break;
    case K_LOOKAHEAD: {
        double d = rw ? lookahead_from_curvature(g_args.f) : O(double(__cdecl*)(float), 0x42fa00)(g_args.f);
        memcpy(&r, &d, 8);
        break;
    }
    case K_RTINFO: rw ? AICar_init_rtinfo(c, 0) : O(V_t, 0x42fa60)(c, 0); break;
    case K_LINEINFO: rw ? AICar_update_line_info(c, 0) : O(V_t, 0x42fb80)(c, 0); break;
    case K_SEGINFO: rw ? AICar_update_segment_info(c, 0) : O(V_t, 0x42fbd0)(c, 0); break;
    case K_MPATH: rw ? AICar_get_mpath_target(c, 0, g_args.f) : O(void(__fastcall*)(AICar*, int, float), 0x42fc50)(c, 0, g_args.f); break;
    case K_REAL: rw ? AICar_get_real_target(c, 0) : O(V_t, 0x4300f0)(c, 0); break;
    default: break;
    }
#undef O
    return r;
}
static uint32_t g_fault_eip, g_fault_code, g_fault_addr;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_eip = (uint32_t)e->ContextRecord->Eip;
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_addr = e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static void reset_run() {
    g_log.n = 0;
    g_alloc_i = 0;
    g_heap_used = 0;
}
static int run_guarded(Kind k, bool rw, uint64_t* ret) {
    reset_run();
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, _PC_24, _MCW_PC);
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] { *ret = call_kind(k, rw); }, fault_filter)) { return 1; }
#else
    __try { *ret = call_kind(k, rw); } __except (fault_filter(GetExceptionInformation())) { return 1; }
#endif
    return 0;
}

// the rewrite's footprint for the world's call
static int footprint_of(Kind k, Footprint& fp) {
    AICar* c = car();
    fp.n = 0; fp.pure = false; fp.replay_only = 0;
    reset_run();
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] {
#else
    __try {
#endif
        switch (k) {
        case K_REPL: fpof_AICar_repl_handler(fp, g_event, 0); break;
        case K_STUFF: fpof_AICar_stuff_event(fp, c, 0, g_args.ev_kind, g_args.ev_value, g_args.text); break;
        case K_GETMSG: fpof_AICar_GetMessage(fp, c, 0, g_msgbuf); break;
        case K_UPDREPLAY: fpof_AICar_UpdateReplay(fp, c, 0, g_p0, g_p1, g_args.u); break;
        case K_COMMON: fpof_AICar_UpdateCommon(fp, c, 0); break;
        case K_MAKEPKT: fpof_AICar_MakeReplayPacket(fp, c, 0, g_pkt); break;
        case K_LOCVEC: fpof_AICar_localify_vector(fp, c, 0, &g_pt2); break;
        case K_LOCLOC: fpof_AICar_localify_loc(fp, c, 0, &g_pt2); break;
        case K_ILPOS: fpof_AICar_get_ilpos(fp, c, 0, &g_ilpos); break;
        case K_RESOLVE: fpof_AICar_ResolveExternalImpulse(fp, c, 0, &g_args.j, &g_args.pt, g_args.type); break;
        case K_FORCE: fpof_AICar_ApplyExternalForce(fp, c, 0, &g_args.j, &g_args.pt, g_args.type); break;
        case K_RESET: fpof_AICar_Reset(fp, c, 0); break;
        case K_FAST: fpof_AICar_fast_mode(fp, c, 0, g_args.text); break;
        case K_SLOW: fpof_AICar_slow_mode(fp, c, 0, g_args.text); break;
        case K_CTOR: fpof_AICar_ctor(fp, c, 0, g_args.ctor_data, g_args.ctor_p); break;
        case K_RESETP: fpof_AICar_reset(fp, c, 0); break;
        case K_DTOR: fpof_AICar_dtor(fp, c, 0); break;
        case K_SUSPEND: fpof_AICar_suspend_adjustment(fp, c, 0, g_args.text); break;
        case K_FOUROFF: fpof_AICar_fire_fouroff(fp, c, 0); break;
        case K_REL_II: fpof_AICar_GetRelSegment_ii(fp, c, 0, g_args.a, g_args.b); break;
        case K_REL_SI: fpof_AICar_GetRelSegment_si(fp, c, 0, g_args.s, g_args.a); break;
        case K_THROTTLE: fpof_AICar_AISetThrottle(fp, c, 0, g_args.f); break;
        case K_ONWATER: case K_OFFGROUND: case K_DAMAGED: fpof_on_water(fp, &c->wheels[0]); break;
        case K_COUNT: fpof_count_wheels(fp, g_args.pred, c->wheels); break;
        case K_STEER: fpof_AICar_AISetSteering(fp, c, 0, g_args.f); break;
        case K_BRAKE: fpof_AICar_AISetBrake(fp, c, 0, g_args.u); break;
        case K_YAW: fpof_AICar_set_yaw_control(fp, c, 0, g_args.on); break;
        case K_TC: fpof_AICar_set_traction_control(fp, c, 0, g_args.on); break;
        case K_ABS: fpof_AICar_set_abs(fp, c, 0, g_args.on); break;
        case K_NOTES: fpof_Notes_clear(fp, (Notes*)g_args.s, 0); break;
        case K_MSG: fpof_AICar_MSG(fp, c, 0, 0); break;
        case K_TELEPORT: fpof_AICar_teleport_to_track(fp, c, 0); break;
        case K_STRANDED: fpof_AICar_check_for_stranded(fp, c, 0); break;
        case K_TOOFAST: fpof_AICar_check_for_too_fast(fp, c, 0); break;
        case K_DAMAGE: fpof_AICar_check_for_damage(fp, c, 0); break;
        case K_INITSEG: fpof_AICar_init_seginfo(fp, c, 0); break;
        case K_LOOKAHEAD: fpof_lookahead_from_curvature(fp, g_args.f); break;
        case K_RTINFO: fpof_AICar_init_rtinfo(fp, c, 0); break;
        case K_LINEINFO: fpof_AICar_update_line_info(fp, c, 0); break;
        case K_SEGINFO: fpof_AICar_update_segment_info(fp, c, 0); break;
        case K_MPATH: fpof_AICar_get_mpath_target(fp, c, 0, g_args.f); break;
        case K_REAL: fpof_AICar_get_real_target(fp, c, 0); break;
        default: break;
        }
#if defined(__GNUC__) && !defined(__clang__)
    })) { return 1; }
#else
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
#endif
    return 0;
}
static bool in_footprint(const Footprint& fp, const uint8_t* p) {
    for (int i = 0; i < fp.n; i++)
        if (p >= (const uint8_t*)fp.r[i].p && p < (const uint8_t*)fp.r[i].p + fp.r[i].n) return true;
    return false;
}

// a name for a byte of the car
static const char* name_of(uint32_t off, char* buf) {
    struct F { uint32_t off; const char* name; };
    static const F f[] = {{0x38, "frame.rot"}, {0x5c, "frame.pos"}, {0x234, "velocity"}, {0x4f8, "yaw_control"}, {0x500, "steering"},
        {0x554, "wheels"}, {0xbe4, "Car"}, {0xde0, "throttle"}, {0xde4, "braking"}, {0xde8, "Car"}, {0xec8, "drive_fn"}, {0xecc, "interact_fn"},
        {0xef0, "slam_steer"}, {0xf00, "pass_offset"}, {0xf0c, "now"}, {0xf10, "speed"}, {0xf18, "center_lat"}, {0xf28, "lat_accel"},
        {0xf2c, "race_fraction"}, {0xf30, "finished"}, {0xf34, "lap"}, {0xf38, "race_begun"}, {0xf44, "progress_seg"}, {0xf48, "steer_damping"},
        {0xf4c, "steer"}, {0xf50, "steer_gain"}, {0xf54, "tc_always"}, {0xf58, "skill"}, {0xf5c, "target_world"}, {0xf64, "target_pos"},
        {0xf6c, "target_vel"}, {0xf74, "target_pos_dir"}, {0xf7c, "target_vel_dir"}, {0xf84, "target_speed"}, {0xf88, "center_pos"},
        {0xfb8, "progress_time"}, {0xfbc, "finish_time"}, {0xfc0, "crash_time"}, {0xfc4, "bump_time"}, {0xfc8, "fouroff_time"},
        {0xfcc, "race_start_time"}, {0xfd0, "freeze_time"}, {0xfd4, "wall_heat"}, {0xfd8, "_fd8"}, {0xfdc, "recover_side"},
        {0xfe0, "offground_cut"}, {0xfe4, "line_update_time"}, {0xfe8, "line"}, {0xfec, "msgs"}, {0x1014, "num_msgs"}, {0x1018, "min_brake"},
        {0x1020, "too_fast"}, {0x1024, "lap_target_time"}, {0x1028, "base_lap_time"}, {0x102c, "lap_time_spread"}, {0x1030, "pace"},
        {0x1034, "schedule_error"}, {0x1038, "lap_clock"}, {0x103c, "throttle_cap"}, {0x1040, "lap_time_offset"}, {0x1044, "line_lap_time"},
        {0x1048, "proxer_info"}, {0x104c, "num_proxer_info"}, {0x1050, "learn_hiatus"}, {0x1054, "seginfo"}, {0x1058, "num_segs"},
        {0x105c, "seg_index"}, {0x1060, "next_seg_index"}, {0x1064, "seg_t"}, {0x1068, "seg_changed/valid"}, {0x106c, "wall_hit_lat"},
        {0x1070, "last_learn_lap"}, {0x1074, "latslam_rel_speed"}, {0x1078, "launch_time"}, {0x107c, "teleport_armed_time"}, {0x1080, "end"}};
    for (int i = (int)(sizeof f / sizeof *f) - 2; i >= 0; i--)
        if (off >= f[i].off) { sprintf(buf, "car.%s+%u", f[i].name, off - f[i].off); return buf; }
    sprintf(buf, "car+0x%x", off);
    return buf;
}

static bool compare(const State& o, const State& n, int it, Kind k) {
    bool same = true;
    char buf[96];
    for (const Block& b : g_blocks) {
        const uint8_t* a = (const uint8_t*)&o + b.off;
        const uint8_t* c = (const uint8_t*)&n + b.off;
        int shown = 0;
        for (size_t i = 0; i + 4 <= b.n; i += 4) {
            uint32_t x, y;
            memcpy(&x, a + i, 4); memcpy(&y, c + i, 4);
            if (x == y) continue;
            if (same) printf("  MISMATCH, world %d (%s)\n", it, kind_names[k]);
            same = false;
            if (shown++ < 8)
                printf("    %-30s original %08x (%.9g)  rewrite %08x (%.9g)\n",
                       b.off == offsetof(State, car) ? name_of((uint32_t)i, buf) : (sprintf(buf, "%s+0x%x", b.name, (unsigned)i), buf),
                       x, bitsf(x), y, bitsf(y));
        }
    }
    if (memcmp(o.statics, n.statics, sizeof o.statics)) {
        if (same) printf("  MISMATCH, world %d (%s)\n", it, kind_names[k]);
        printf("    a static differs\n");
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

// the bytes the original changed, against the rewrite's footprint (reported once a world)
static int check_footprint(const State& start, const State& orig, const Footprint& fp, int it, Kind k) {
    if (fp.replay_only) return 0;
    char buf[96];
    g_blocks[0].live = g_carbuf;
    for (const Block& b : g_blocks) {
        const uint8_t* a = (const uint8_t*)&start + b.off;
        const uint8_t* c = (const uint8_t*)&orig + b.off;
        for (size_t i = 0; i < b.n; i++)
            if (a[i] != c[i] && !in_footprint(fp, (const uint8_t*)b.live + i)) {
                printf("  FOOTPRINT, world %d (%s): %s changed outside it\n", it, kind_names[k],
                       b.off == offsetof(State, car) ? name_of((uint32_t)i, buf) : (sprintf(buf, "%s+0x%x", b.name, (unsigned)i), buf));
                return 1;
            }
    }
    if (memcmp(start.statics, orig.statics, 8) && !fp.replay_only) {
        printf("  FOOTPRINT, world %d (%s): a static changed\n", it, kind_names[k]);
        return 1;
    }
    return 0;
}

// ---- coverage --------------------------------------------------------------------------------------------------
enum { C_MPATH_NOLINE, C_MPATH_FOUROFF, C_MPATH_LAUNCH, C_MPATH_RACE, C_MPATH_NOTES_LAT, C_MPATH_CONTACT, C_MPATH_FLIP,
       C_TOOFAST_EARLY, C_TOOFAST_BEHIND, C_TOOFAST_EDGE, C_TOOFAST_SET, C_TOOFAST_CAP_MID, C_TOOFAST_NULLBEAD,
       C_STRANDED_PROGRESS, C_STRANDED_TELEPORT_TRY, C_STRANDED_TELEPORTED, C_TELEPORT_ARM, C_TELEPORT_FIRE, C_TELEPORT_MOVED,
       C_BRAKE_PANIC, C_BRAKE_NOTALL, C_BRAKE_BLEND, C_STEER_CLAMP, C_STEER_DAMP, C_STEER_SAT, C_THROTTLE_WATER, C_THROTTLE_OFFG,
       C_DAMAGE_FROZEN, C_DAMAGE_TELEPORT, C_SEG_CHANGED, C_SEG_NULL, C_CTOR_LINE, C_CTOR_NOLINE, C_CTOR_MISMATCH, C_DTOR_LEARN,
       C_RESOLVE_WALL, C_RESOLVE_CRASH, C_FORCE_CAR, C_FORCE_WALL, C_RTINFO_FINISH, C_REAL_TINY, C_MSG_FULL, C_UPDREPLAY_MSGS,
       C_BOTH_FAULT_NULLBEAD, N_COV };
static const char* const cov_names[N_COV] = {"mpath no line", "mpath four-off blend", "mpath launch blend", "mpath racing",
    "mpath lateral note", "mpath contact pass", "mpath contact flip", "too_fast early out", "too_fast behind schedule", "too_fast lap edge",
    "too_fast set", "too_fast cap blend", "too_fast NULL bead", "stranded progress", "stranded teleport tried", "stranded teleported",
    "teleport armed", "teleport fired", "teleport moved", "brake panic", "brake not all down", "brake blend", "steer slew clamp",
    "steer damping up", "steer saturated", "throttle water", "throttle offground", "damage frozen", "damage teleport",
    "segment changed", "segment NULL bead", "ctor with line", "ctor no line", "ctor wrong car", "dtor learn notes", "impulse wall",
    "impulse crash", "force car contact", "force wall", "rtinfo finish stamp", "real target tiny", "_MSG full", "replay msgs",
    "both faulted (NULL bead)"};
static int g_cov[N_COV];
static bool log_has(const Log& l, uint32_t v) { for (int i = 0; i < l.n && i < 256; i++) if (l.e[i] == v) return true; return false; }
static void coverage(Kind k, const State& s, const State& o) {
    const AICar* sc = (const AICar*)s.car;
    const AICar* oc = (const AICar*)o.car;
    switch (k) {
    case K_MPATH: case K_REAL: {
        if (!sc->line) g_cov[C_MPATH_NOLINE]++;
        const float now = k == K_MPATH ? g_args.f : sc->now;
        if (!(D(now) - sc->fouroff_time >= 12.0)) g_cov[C_MPATH_FOUROFF]++;
        else if (!(D(now) - sc->race_start_time >= D(sc->launch_time))) g_cov[C_MPATH_LAUNCH]++;
        else g_cov[C_MPATH_RACE]++;
        if (!sc->finished && sc->seg_valid && (fbits(sc->seginfo[sc->seg_index].lateral_note) & 0x7fffffff)) g_cov[C_MPATH_NOTES_LAT]++;
        for (int i = 0; i < 10; i++) {
            if (oc->msgs[i] == msg_tab(MSG_CONTACT) && i < oc->num_msgs) g_cov[C_MPATH_CONTACT]++;
            if (oc->msgs[i] == msg_tab(MSG_CONTACT_FLIP) && i < oc->num_msgs) g_cov[C_MPATH_FLIP]++;
        }
        if (k == K_REAL && fabs(D(oc->target_vel.x)) < 1e-6 && fabs(D(oc->target_vel.z)) < 1e-6) g_cov[C_REAL_TINY]++;
        break;
    }
    case K_TOOFAST:
        if (fbits(sc->speed) <= 0x41b1c71c || sc->finished || sc->lap == 0) g_cov[C_TOOFAST_EARLY]++;
        else if (!sc->line || !((IdealLine*)s.line)->bead_seg) g_cov[C_TOOFAST_NULLBEAD]++;
        else {
            const int at = (int)(((IdealLine*)s.line)->bead_seg - &g_segs[0]);
            if (at < 3 || at > sc->num_segs - 3) g_cov[C_TOOFAST_EDGE]++;
            else if (fbits(oc->too_fast) == 0) g_cov[C_TOOFAST_BEHIND]++;
            else g_cov[C_TOOFAST_SET]++;
        }
        if (fbits(oc->too_fast) != 0 && fbits(oc->too_fast) < 0x3f800000) g_cov[C_TOOFAST_CAP_MID]++;
        break;
    case K_STRANDED:
        if (oc->progress_seg != sc->progress_seg) g_cov[C_STRANDED_PROGRESS]++;
        if (log_has(o.log, 0x6a3e0000u) || fbits(oc->teleport_armed_time) != fbits(sc->teleport_armed_time)) g_cov[C_STRANDED_TELEPORT_TRY]++;
        if (log_has(o.log, 0xbead0000u)) g_cov[C_STRANDED_TELEPORTED]++;
        break;
    case K_TELEPORT:
        if (fbits(oc->teleport_armed_time) == fbits(sc->now)) g_cov[C_TELEPORT_ARM]++;
        if (log_has(o.log, 0xde170001u)) g_cov[C_TELEPORT_FIRE]++;
        if (o.ret) g_cov[C_TELEPORT_MOVED]++;
        break;
    case K_BRAKE:
        for (int i = 0; i < 10; i++) if (i < oc->num_msgs && oc->msgs[i] == msg_tab(MSG_PANIC_BRAKE)) { g_cov[C_BRAKE_PANIC]++; break; }
        if (sc->wheels[0].on_ground + sc->wheels[1].on_ground + sc->wheels[2].on_ground + sc->wheels[3].on_ground != 4) g_cov[C_BRAKE_NOTALL]++;
        if (D(sc->braking) > D(oc->braking) || (D(sc->braking) > 0 && fbits(oc->braking) != 0 && fbits(oc->braking) != 0x3f800000)) g_cov[C_BRAKE_BLEND]++;
        break;
    case K_STEER: {
        const float mx = (float)(2.0 / (D(*(float*)(s.skill + 0x28)) * 31.25));
        if (fabs(D(g_args.f) - sc->steer) > mx) g_cov[C_STEER_CLAMP]++;
        if (fbits(oc->steer_damping) != fbits(sc->steer_damping)) g_cov[C_STEER_DAMP]++;
        if (fabs(D(oc->steer)) == 1.0) g_cov[C_STEER_SAT]++;
        break;
    }
    case K_THROTTLE:
        for (int i = 0; i < 10; i++) {
            if (i < oc->num_msgs && oc->msgs[i] == msg_tab(MSG_WATER_CUT)) g_cov[C_THROTTLE_WATER]++;
            if (i < oc->num_msgs && oc->msgs[i] == msg_tab(MSG_OFFGROUND_CUT)) g_cov[C_THROTTLE_OFFG]++;
        }
        break;
    case K_DAMAGE:
        if (fbits(oc->freeze_time) != fbits(sc->freeze_time)) g_cov[C_DAMAGE_FROZEN]++;
        if (log_has(o.log, 0x6a3e0000u) || fbits(oc->teleport_armed_time) != fbits(sc->teleport_armed_time)) g_cov[C_DAMAGE_TELEPORT]++;
        break;
    case K_SEGINFO:
        if (oc->seg_changed && oc->seg_valid && !sc->seg_valid) g_cov[C_SEG_CHANGED]++;
        else if (oc->seg_changed && oc->seg_index != sc->seg_index) g_cov[C_SEG_CHANGED]++;
        if (sc->line && !((IdealLine*)s.line)->bead_seg) g_cov[C_SEG_NULL]++;
        break;
    case K_CTOR:
        if (oc->line) g_cov[C_CTOR_LINE]++; else g_cov[C_CTOR_NOLINE]++;
        if (log_has(o.log, 0x10900000u)) g_cov[C_CTOR_MISMATCH]++;
        break;
    case K_DTOR: if (log_has(o.log, 0x7ec07d00u)) g_cov[C_DTOR_LEARN]++; break;
    case K_RESOLVE:
        if (g_args.type != 0 && g_args.type != 0x6d) g_cov[C_RESOLVE_WALL]++;
        if (fbits(oc->crash_time) != fbits(sc->crash_time)) g_cov[C_RESOLVE_CRASH]++;
        break;
    case K_FORCE:
        if (g_args.type == 0x6d && fbits(oc->slam_steer) != fbits(sc->slam_steer)) g_cov[C_FORCE_CAR]++;
        if (g_args.type == 0x6e && fbits(oc->slam_steer) != fbits(sc->slam_steer)) g_cov[C_FORCE_WALL]++;
        break;
    case K_RTINFO: if (fbits(oc->finish_time) != fbits(sc->finish_time)) g_cov[C_RTINFO_FINISH]++; break;
    case K_MSG: if (sc->num_msgs >= 10 || sc->num_msgs < 0) g_cov[C_MSG_FULL]++; break;
    case K_UPDREPLAY: if (oc->num_msgs > 0) g_cov[C_UPDREPLAY_MSGS]++; break;
    default: break;
    }
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
// a world where check_for_too_fast gets past its early out, with a line
static void too_fast_world() {
    for (;;) {
        randomize_world();
        randomize_args();
        AICar* c = car();
        if (!c->line) continue;
        c->speed = range(22.3f, 80.0f);
        c->finished = 0;
        if (!c->lap) c->lap = 1;
        return;
    }
}
static void run_fix_tests() {
    static State a, b, ra;
    long same = 0, not_found = 0, orig_faults = 0;
    // check_for_too_fast on a lost bead: the fixed one asks reset_bead_position to find it (the stub puts it at a
    // scripted segment) and goes on as the original does on a bead already there
    for (int it = 0; it < 20000; it++) {
        too_fast_world();
        AICar* c = car();
        const int k = (int)(rnd() % g_nsegs);
        const float t = uni();
        g_script.bead_after_reset = chance(15) ? -1 : k;
        g_script.bead_t_after = t;
        line()->bead_seg = 0;
        save(a);
        uint64_t r;
        const int fn = run_guarded(K_TOOFAST, true, &r);
        save(ra);
        ra.log = g_log;
        FIXCHECK(!fn, "check_for_too_fast faulted on a NULL bead (world %d)", it);
        FIXCHECK(log_has(ra.log, 0xbead0000u), "check_for_too_fast didn't ask for the bead (world %d)", it);
        load(a);
        const int fo = run_guarded(K_TOOFAST, false, &r);
        if (fo) orig_faults++;
        if (g_script.bead_after_reset < 0) {
            not_found++;
            FIXCHECK(fbits(((AICar*)ra.car)->too_fast) == 0, "no bead: too_fast isn't 0");
            continue;
        }
        // the original, on the bead where reset_bead_position put it
        load(a);
        line()->bead_seg = &g_segs[k];
        line()->bead_t = t;
        save(b);
        const int fb = run_guarded(K_TOOFAST, false, &r);
        save(b);
        if (fb) continue;
        same++;
        FIXCHECK(!memcmp(ra.car, b.car, CAR_BUF) && !memcmp(ra.line, b.line, sizeof ra.line),
                 "check_for_too_fast: the found bead's result differs from the original's on it (world %d)", it);
        (void)c;
    }
    printf("check_for_too_fast, NULL bead: the original faulted %ld times; the fixed one found it (%ld as the original's on "
           "it) or not (%ld, not too fast)\n", orig_faults, same, not_found);
    FIXCHECK(orig_faults > 10000, "the original didn't fault on NULL beads");
    // stuff_event: texts of 32 characters and more, cut to 31 (the original would run over its stack: not run)
    static char text[4096];
    long cut = 0;
    for (int it = 0; it < 4000; it++) {
        randomize_world();
        randomize_args();
        const int len = it % 4 == 0 ? 31 + (int)(rnd() % 3) : 32 + (int)(rnd() % 4000);
        for (int i = 0; i < len; i++) text[i] = (char)('A' + rnd() % 26);
        text[len] = 0;
        reset_run();
        unsigned cw;
        _controlfp_s(&cw, _PC_24, _MCW_PC);
        bool fault = false;
#if defined(__GNUC__) && !defined(__clang__)
        if (vp_try([&] { AICar_stuff_event(car(), 0, g_args.ev_kind & 1, g_args.ev_value, text); })) { fault = true; }
#else
        __try { AICar_stuff_event(car(), 0, g_args.ev_kind & 1, g_args.ev_value, text); } __except (EXCEPTION_EXECUTE_HANDLER) { fault = true; }
#endif
        FIXCHECK(!fault, "stuff_event faulted on a %d-character text", len);
        // the event as the replay got it: {kind, car, value, text[32]} after the stub's (0xadde0000, type, size)
        FIXCHECK(g_log.n >= 3 + 11 && g_log.e[0] == 0xadde0000u && g_log.e[2] == 0x2c, "stuff_event: no event");
        char got[32];
        memcpy(got, &g_log.e[3 + 3], 32);
        const int want = len < 31 ? len : 31;
        FIXCHECK(!memcmp(got, text, want) && got[want] == 0, "stuff_event: a %d-character text isn't cut to %d", len, want);
        if (len >= 32) cut++;
    }
    printf("stuff_event: %ld texts of 32 characters or more cut to 31, none faulted\n", cut);
}
#endif

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 200000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const char* only = argc > 3 ? argv[3] : 0;          // just this function (its name in the table below)
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_aicar1.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    patch_jmp(0x0042bc80, (void*)&stub_time);
    patch_jmp(0x00420df0, (void*)&stub_tracknum);
    patch_jmp(0x00420d90, (void*)&stub_getskill);
    patch_jmp(0x00462720, (void*)&stub_carentry);
    patch_jmp(0x004da350, (void*)&stub_stricmp);
    patch_jmp(0x00411150, (void*)&stub_logreport);
    patch_jmp(0x004112b0, (void*)&stub_logpanic);
    patch_jmp(0x0042d8b0, (void*)&stub_addevent);
    patch_jmp(0x0042d960, (void*)&stub_install);
    patch_jmp(0x0042d990, (void*)&stub_uninstall);
    patch_jmp(0x0041d220, (void*)&stub_register);
    patch_jmp(0x0041d2e0, (void*)&stub_unregister);
    patch_jmp(0x0041c2e0, (void*)&stub_getline);
    patch_jmp(0x004140e0, (void*)&stub_memalloc);
    patch_jmp(0x00414390, (void*)&stub_delete);
    patch_jmp(0x00424b70, (void*)&stub_driverdata);
    patch_jmp(0x00424be0, (void*)&stub_release);
    patch_jmp(0x00423780, (void*)&stub_record_notes);
    patch_jmp(0x00465ae0, (void*)&stub_terrain_xz);
    patch_jmp(0x0040a3d0, (void*)&stub_gamestate);
    patch_jmp(0x004214c0, (void*)&stub_update_car_info);
    patch_jmp(0x00421520, (void*)&stub_reset_bead);
    patch_jmp(0x00421a70, (void*)&stub_rabbit);
    patch_jmp(0x004307e0, (void*)&stub_init_rt_lat);
    patch_jmp(0x004444e0, (void*)&stub_localcar_ctor);
    patch_jmp(0x00444540, (void*)&stub_localcar_dtor);
    patch_jmp(0x00439f90, (void*)&stub_car_reset);
    patch_jmp(0x004391b0, (void*)&stub_car_common);
    patch_jmp(0x00438ef0, (void*)&stub_car_getmessage);
    patch_jmp(0x00438cd0, (void*)&stub_car_makepacket);
    patch_jmp(0x00438950, (void*)&stub_car_updatereplay);
    patch_jmp(0x00437120, (void*)&stub_car_resolve);
    patch_jmp(0x00436a90, (void*)&stub_car_force);
    for (int i = 0; i < 32; i++) g_deity_vt[i] = (void*)&stub_deity_bad;
    g_deity_vt[0x20 / 4] = (void*)&stub_deity_laps;
    g_deity_vt[0x3c / 4] = (void*)&stub_deity_teleport;
    g_deity_vt[0x48 / 4] = (void*)&stub_deity_lap;
    g_deity_obj.vt = g_deity_vt;
    *(void**)0x005218ac = &g_deity_obj;
    *(void**)0x004edd70 = g_rdeity;
    g_carbuf = (uint8_t*)VirtualAlloc(0, CAR_BUF, MEM_COMMIT, PAGE_READWRITE);

    static State start, orig, rew;
    static Footprint fp;
    int differ = 0, faults = 0, fault_mismatch = 0, wild_runs = 0, wild_differ = 0, fp_bad = 0, replay_only = 0, fp_faults = 0;
    int per_kind[N_KINDS] = {0}, per_kind_bad[N_KINDS] = {0}, per_kind_fault[N_KINDS] = {0};
    int same_fault_addr = 0;
    int fixed = 0;                                    // (FIX_TESTS: the original faulted, the fixed one ran)
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weight[i];
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), k = 0;
        while (pick >= kind_weight[k]) pick -= kind_weight[k++];
        if (only) for (k = 0; k < N_KINDS && strcmp(kind_names[k], only); k++) {}
        if (k == N_KINDS) { printf("no function %s\n", only); return 2; }
        Kind kind = (Kind)k;
        randomize_world();
        randomize_args();
        bool wild = chance(10);
        if (wild) { wildify(); wild_runs++; }
        save(start);
        if (footprint_of(kind, fp)) { fp_faults++; fp.replay_only = "the footprint faulted"; }
        load(start);
        if (fp.replay_only) replay_only++;
        int fo = run_guarded(kind, false, &orig.ret);
        const uint32_t fo_addr = g_fault_addr, fo_code = g_fault_code;
        save(orig);
        orig.log = g_log;
        load(start);
        int fn = run_guarded(kind, true, &rew.ret);
        save(rew);
        rew.log = g_log;
        per_kind[kind]++;
#ifdef FIX_TESTS
        {   // the fixed rewrite: its changes inside its footprint; a lost bead in check_for_too_fast is the fix
            fp_bad += check_footprint(start, rew, fp, it, kind);
            const AICar* sc = (const AICar*)start.car;
            if (fo && !fn && kind == K_TOOFAST && sc->line && !((IdealLine*)start.line)->bead_seg) { fixed++; per_kind[kind]++; continue; }
        }
#endif
        if (fo || fn) {
            if (fo != fn) {
                printf("  world %d (%s): original %s, rewrite %s (fault %08x at %08x, address %08x)\n", it, kind_names[kind], fo ? "faulted" : "ran",
                       fn ? "faulted" : "ran", fo ? fo_code : g_fault_code, g_fault_eip, fo ? fo_addr : g_fault_addr);
                differ++; per_kind_bad[kind]++; fault_mismatch++;
            } else {
                faults++;
                per_kind_fault[kind]++;
                if (fo_addr == g_fault_addr && fo_code == g_fault_code) same_fault_addr++;
                else if (faults - same_fault_addr <= 5)
                    printf("  world %d (%s): both faulted, original %08x at %08x, rewrite %08x at %08x\n", it, kind_names[kind], fo_code, fo_addr, g_fault_code, g_fault_addr);
                const AICar* sc = (const AICar*)start.car;
                if (kind == K_TOOFAST && sc->line && !((IdealLine*)start.line)->bead_seg) g_cov[C_BOTH_FAULT_NULLBEAD]++;
            }
            continue;
        }
        if (!compare(orig, rew, it, kind)) {
            differ++;
            per_kind_bad[kind]++;
            if (wild) wild_differ++;
            if (differ >= 25) { printf("stopping after 25 differing worlds\n"); break; }
        }
        fp_bad += check_footprint(start, orig, fp, it, kind);
        coverage(kind, start, orig);
    }
    printf("%d worlds (%d wild): %d differ (%d of them wild, %d a fault in one only), %d faulted in both (%d at the same address), "
           "%d changed bytes outside the footprint, %d replay-only (%d footprints faulted)\n",
           iterations, wild_runs, differ, wild_differ, fault_mismatch, faults, same_fault_addr, fp_bad, replay_only, fp_faults);
    printf("per function (worlds / differing / both faulted):\n");
    for (int i = 0; i < N_KINDS; i++) printf("  %-26s %6d / %d / %d\n", kind_names[i], per_kind[i], per_kind_bad[i], per_kind_fault[i]);
#ifdef FIX_TESTS
    printf("fixed: %d worlds where the original faulted on a NULL bead and the fixed one ran\n", fixed);
    run_fix_tests();
    printf("fix tests: %d failures\n", g_fix_fail);
    if (g_fix_fail) differ++;
#else
    (void)fixed;
#endif
    printf("coverage:\n");
    for (int i = 0; i < N_COV; i++) printf("  %-28s %d\n", cov_names[i], g_cov[i]);
    return differ || fp_bad ? 1 : 0;
}
