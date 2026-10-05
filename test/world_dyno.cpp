// world_dyno.cpp -- the physics-object base classes and simple objects (hook/phys_dyno.cpp) against the
// originals, on random worlds, outside the game (docs/PORTING.md, checking step 3).
//
//   build (from the repo root, in an x86 VS prompt; linked at 0x10000000 so 0x400000 is free):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_dyno.cpp
//        /Fe:world_dyno.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:    world_dyno.exe [worlds] [seed]
//   the fixes (docs/PORTING.md, "Fixes"): the same with /DFIX_TESTS (or /DVP_TEST_FIXES) -- the rewrites as the game builds them,
//   against the original with vrmod's obstacle-wake patch (Obstacle::Reset's `ret` NOPed into Perturb)
//
// Like test/fuzz.cpp it loads out\race_v10.exe's sections at 0x400000 in a child process that reserved the
// range before its heap existed. Each world is one block of memory holding a physics object of a random
// class (PhobDyno, Obstacle, Wobble, Ball -- half the time with a copy of its vtable whose
// ResolveExternalImpulse logs its call and may change the impulse weight), its real Sphere / Cube volumes
// (built by the original constructors), a CheckPoint, packets, frames, a PhobData, a static object, a
// message buffer, a heap and a fake Deity; plus the game's ball table. Every rewrite is called on it with
// random arguments, from the same starting state as the original, and the whole block, the ball table, the
// return value and the stubs' call logs must come out bit for bit the same; and every byte the original
// changed must lie in the rewrite's footprint (unless it is replay_only). The originals' callees run for
// real from the image (MatrixConcat, CrossProduct, the volumes' CollideGround, get_impulse_magnitude, ...)
// except what reaches outside the world, patched with a jump to a logging stub:
//   TerrainGetIntersection, TerrainGetSphereIntersection   scripted hits (height, normal, surface incl. water)
//   Collide, CollideWater                                  crash / splash sounds: logged
//   PhysicsGetTime                                         a scripted clock
//   MemAlloc, operator delete                              a bump heap inside the world (null sometimes); logged
//   LogReport, PhysTaskFindStaticObject, HackGetCarIndex, GetCarFileName, the Deity's +0x34
// Each check runs at the physics thread's 24-bit precision, or (1 in 8) the main thread's 53.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>
#include <type_traits>
#if defined(FIX_TESTS) && !defined(VP_TEST_FIXES)
#define VP_TEST_FIXES           // (the flag the other harnesses use)
#endif
#ifndef VP_TEST_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#endif
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

// ---- class sizes by vtable (state_layout.inc), and the logging vtable copies -----------------------------
static void* g_vt_copy[4][16];                                // PhobDyno, Obstacle, Wobble, Ball
static const uint32_t k_vt_orig[4] = {0x004dc690, 0x004dc0c8, 0x004dc108, 0x004dc3b0};
static const uint32_t k_vt_size[4] = {1144, 1200, 1204, 1152};
static int g_fp_unknown;
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) {
    uint32_t vt = *(uint32_t*)obj;
    struct { uint32_t vt, size; } k[] = {{0x004dc690, 1144}, {0x004dc0c8, 1200}, {0x004dc108, 1204}, {0x004dc3b0, 1152},
                                         {0x004dc178, 108},  {0x004dc1b0, 108},  {0x004dc350, 132},  {0x004dbd18, 64},
                                         {0x004dbd38, 68},   {0x004dbdd8, 48}};
    for (auto& c : k)
        if (c.vt == vt) { add(obj, c.size, what); return; }
    for (int i = 0; i < 4; i++)
        if (vt == (uint32_t)(uintptr_t)g_vt_copy[i]) { add(obj, k_vt_size[i], what); return; }
    printf("footprint: %s at %p has an unknown class (vtable %08x)\n", what, obj, vt);
    g_fp_unknown++;
}

#include "../hook/phys_dyno.cpp"
#include "vrmod_image.h"
// VP_VRMOD=1: vrmod's hornball.py tunings -- Ball::Throw's four .rdata floats (cooldown, speed, ahead, up) -- set to
// random player values before each check of it (docs/FIXES.md, "vrmod's patches": the rewrite reads them there)
static bool g_vrmod;

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
static int g_special_pct = 3;
static float special() {
    switch (rnd() % 8) {
    case 0: return fbits(0x7fc00000u | (rnd() & 0x3fffff));
    case 1: return fbits(0xffc00000u | (rnd() & 0x3fffff));
    case 2: return fbits(0x7f800001u + (rnd() & 0x3ffffe));
    case 3: return fbits(0x7f800000u | (rnd() & 0x80000000u));
    case 4: return fbits(rnd() & 0x807fffffu);
    case 5: return (rnd() & 1) ? 0.0f : -0.0f;
    case 6: return fbits((rnd() & 0x80000000u) | 0x7f7fffffu);
    default: return fbits(rnd());
    }
}
static float sp(float x) { return (int)(rnd() % 100) < g_special_pct ? special() : x; }
static void rand_p3(P3* p, float lo, float hi) { p->x = sp(rf(lo, hi)); p->y = sp(rf(lo, hi)); p->z = sp(rf(lo, hi)); }
static void rotation(M3* m) {
    double yaw = rf(-3.1416f, 3.1416f), pitch = rf(-3.1416f, 3.1416f), roll = rf(-3.1416f, 3.1416f);
    if (chance(60)) { pitch *= 0.1; roll *= 0.1; }
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), spp = sin(pitch), cr = cos(roll), sr = sin(roll);
    double R[9] = {cy * cr + sy * spp * sr, cp * sr, -sy * cr + cy * spp * sr, -cy * sr + sy * spp * cr, cp * cr,
                   sy * sr + cy * spp * cr, sy * cp, -spp, cy * cp};
    for (int i = 0; i < 9; i++) m->m[i] = sp((float)R[i]);
    if (chance(5)) m->m[ri(0, 8)] += rf(-0.1f, 0.1f);       // not quite orthonormal
}
static void rand_frame(Frame* f) { rotation(&f->rot); rand_p3(&f->pos, -2000, 2000); }

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
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// ---- the world ------------------------------------------------------------------------------------------------
enum : uint32_t {
    W_OBJ = 0x0000, W_OBJ2 = 0x0500, W_V0 = 0x0a00, W_V1 = 0x0a80, W_CP = 0x0b00, W_PA = 0x0c00, W_PB = 0x0c40,
    W_PK = 0x0c80, W_FA = 0x0cc0, W_FB = 0x0cf0, W_FO = 0x0d20, W_P3 = 0x0d60, W_DATA = 0x0e00, W_SO = 0x0e80,
    W_HEAP = 0x1000, W_HEAP_END = 0x2000, W_MSG = 0x2000, W_DEITY = 0x2300, W_SIZE = 0x2400,
};
static uint8_t* g_w;
static uint8_t* at(uint32_t off) { return g_w + off; }
static PhobDyno* obj() { return (PhobDyno*)at(W_OBJ); }
static P3* p3(int i) { return (P3*)at(W_P3 + 16 * i); }
static Ball** const k_balls = (Ball**)0x00522168;
static void** const k_deity = (void**)0x005218ac;
static const char* part_of(uint32_t off, uint32_t* rel) {
    struct { uint32_t o, n; const char* name; } parts[] = {
        {W_OBJ, 0x500, "obj"}, {W_OBJ2, 0x500, "obj2"}, {W_V0, 0x80, "volume0"}, {W_V1, 0x80, "volume1"},
        {W_CP, 0x100, "checkpoint"}, {W_PA, 0x40, "packetA"}, {W_PB, 0x40, "packetB"}, {W_PK, 0x40, "packet"},
        {W_FA, 0x30, "frameA"}, {W_FB, 0x30, "frameB"}, {W_FO, 0x30, "frameOut"}, {W_P3, 0xa0, "p3 args"},
        {W_DATA, 0x80, "PhobData"}, {W_SO, 0x80, "static object"}, {W_HEAP, 0x1000, "heap"}, {W_MSG, 0x300, "message"},
        {W_DEITY, 0x100, "deity"}};
    for (auto& p : parts)
        if (off >= p.o && off < p.o + p.n) { *rel = off - p.o; return p.name; }
    *rel = off;
    return "world";
}

// ---- the stubs: a script, consumed in order; a log --------------------------------------------------------
struct Hit { uint8_t hit; float h, dx, dz, frac; P3 n; int32_t surface; };
struct Script {
    Hit terrain[8];
    float time[4];
    uint8_t alloc_fail[4];
    int car_index;
    uint8_t plane;
    uint8_t resolve_scale[4];
};
static Script g_script;
static bool g_force_time;
static float g_forced_time;
static int g_ti, g_tmi, g_ai, g_ri;
static uint32_t g_heap_used;
enum { LOG_MAX = 1024 };
static uint32_t g_log[2][LOG_MAX];
static int g_nlog[2], g_pass;
static void log_put(uint32_t v) {
    if (g_nlog[g_pass] < LOG_MAX) g_log[g_pass][g_nlog[g_pass]] = v;
    g_nlog[g_pass]++;
}
static void log_p3(const P3* p) { log_put(ubits(p->x)); log_put(ubits(p->y)); log_put(ubits(p->z)); }
static uint32_t off_of(const void* p) { return (uint32_t)((uintptr_t)p - (uintptr_t)g_w); }

static const Hit& next_hit() { return g_script.terrain[g_ti++ & 7]; }
static uint8_t __cdecl stub_terrain(P3* start, P3* end, P3* hit, P3* n, int32_t* surface) {
    log_put(0x7e000001); log_p3(start); log_p3(end);
    const Hit& t = next_hit();
    if (!t.hit) return 0;
    hit->x = end->x + t.dx; hit->y = end->y + t.h; hit->z = end->z + t.dz;
    *n = t.n;
    *surface = t.surface;
    return 1;
}
static uint8_t __cdecl stub_terrain_sphere(P3* c, uint32_t r, P3* point, P3* n, int32_t* surface) {
    log_put(0x7e000002); log_p3(c); log_put(r);
    const Hit& t = next_hit();
    if (!t.hit) return 0;
    point->x = c->x + t.dx; point->y = c->y - fbits(r) * t.frac; point->z = c->z + t.dz;
    *n = t.n;
    *surface = t.surface;
    return 1;
}
static void __cdecl stub_collide(void* a, void* b, const P3* p, const P3* i, const P3* n) {
    log_put(0xc0000001); log_put(off_of(a)); log_put(off_of(b)); log_p3(p); log_p3(i); log_p3(n);
}
static void __cdecl stub_collide_water(void* v, P3* p, const P3* vel) {
    log_put(0xc0000002); log_put(off_of(v)); log_p3(p); log_p3(vel);
}
static float __cdecl stub_time() { log_put(0x71000000); return g_script.time[g_tmi++ & 3]; }
static void* __cdecl stub_alloc(int n) {
    log_put(0xa1000000); log_put((uint32_t)n);
    if (g_script.alloc_fail[g_ai++ & 3]) return 0;
    uint32_t o = (g_heap_used + 15) & ~15u;
    if (W_HEAP + o + (uint32_t)n > W_HEAP_END) return 0;
    g_heap_used = o + n;
    return at(W_HEAP + o);
}
static void __cdecl stub_delete(void* p) { log_put(0xde000000); log_put(off_of(p)); }
static void __cdecl stub_logreport(const char* fmt, ...) { log_put(0x10000000); log_put((uint32_t)(uintptr_t)fmt); }
static void* __cdecl stub_find_static(int i) { log_put(0x5f000000); log_put((uint32_t)i); return at(W_SO); }
static int __cdecl stub_car_index() { log_put(0xca000000); return g_script.car_index; }
static const char* __cdecl stub_car_name(int i) {
    log_put(0xca000001); log_put((uint32_t)i);
    return g_script.plane ? (i & 1 ? "PLANE" : "plane") : (i & 1 ? "viper" : "planes");
}
static void __fastcall stub_deity(void* self, int, void* arg) {
    log_put(0xde170000); log_put(off_of(self)); log_put(off_of(arg));
}
static void* g_deity_vt[16];
static void __fastcall stub_resolve(PhobDyno* self, int, const P3* imp, const P3* pt, int surface) {
    log_put(0x4e500000); log_put(off_of(self)); log_p3(imp); log_p3(pt); log_put((uint32_t)surface);
    if (g_script.resolve_scale[g_ri++ & 3]) self->impulse_weight = self->impulse_weight * 2.0f + 0.5f;
}

static void make_script() {
    Script& s = g_script;
    for (int i = 0; i < 8; i++) {
        Hit& t = s.terrain[i];
        t.hit = chance(85);
        t.h = chance(10) ? rf(-3, 3) : rf(-0.5f, 0.5f);
        t.dx = rf(-0.05f, 0.05f);
        t.dz = rf(-0.05f, 0.05f);
        t.frac = rf(0.3f, 1.3f);
        double nx, ny, nz;
        int k = rnd() % 10;
        if (k < 6) { nx = rf(-0.2f, 0.2f); nz = rf(-0.2f, 0.2f); ny = 1; }
        else if (k < 9) { nx = rf(-1, 1); nz = rf(-1, 1); ny = rf(-0.3f, 1); }
        else { nx = 0; ny = 1; nz = 0; }
        double l = sqrt(nx * nx + ny * ny + nz * nz);
        t.n.x = sp((float)(nx / l)); t.n.y = sp((float)(ny / l)); t.n.z = sp((float)(nz / l));
        static const int surfaces[] = {0, 0, 0, 1, 5, 10, 14, 14, 14, 20, 100, -1};
        t.surface = surfaces[rnd() % (sizeof surfaces / sizeof *surfaces)];
        if (chance(5)) t.h = sp(special());
    }
    for (int i = 0; i < 4; i++) s.time[i] = g_force_time ? g_forced_time : sp(chance(50) ? rf(0, 600) : rf(0, 5));
    for (int i = 0; i < 4; i++) s.alloc_fail[i] = chance(8);
    s.car_index = ri(-2, 20);
    s.plane = chance(40);
    for (int i = 0; i < 4; i++) s.resolve_scale[i] = chance(30);
}

// ---- building the world ---------------------------------------------------------------------------------------
typedef void*(__fastcall* SphereCtor_t)(void*, int, const Frame*, float a, float b, const P3* c, float r, PhobDyno*);
static void build_volume(uint8_t* v, PhobDyno* owner) {
    memset(v, 0, 0x80);
    P3 c;
    rand_p3(&c, -1.5f, 1.5f);
    float r = sp(rf(0.2f, 2.5f)), a = sp(rf(1e4f, 1e6f)), b = sp(rf(0.1f, 1.0f));
    if (chance(60)) ((SphereCtor_t)0x00432340)(v, 0, &owner->frame, a, b, &c, r, owner);
    else ((SphereCtor_t)0x00432930)(v, 0, &owner->frame, a, b, &c, r, owner);
    CollisionVolume* cv = (CollisionVolume*)v;
    cv->surface_type = chance(70) ? 100 : ri(0, 20);
    ((SphereVolume*)v)->_38 = chance(20);
    ((SphereVolume*)v)->_3c = ubits(owner->frame.pos.y + rf(-3, 3));
    ((void(__fastcall*)(void*, int))VFN(v, 4, void))(v, 0);          // Update: the world centre
}

static int g_class;                                                   // 0 PhobDyno, 1 Obstacle, 2 Wobble, 3 Ball
static void build_dyno(uint8_t* o, int cls) {
    for (int i = 0; i < 0x500; i += 4) { float x = rf(-100, 100); memcpy(o + i, &x, 4); }
    PhobDyno* d = (PhobDyno*)o;
    d->vtable = chance(50) ? (void**)(uintptr_t)k_vt_orig[cls] : g_vt_copy[cls];
    d->_004 = 0;
    d->_008 = ri(0, 0x100);
    d->_00c = rnd();
    d->tick_count = (uint16_t)rnd();
    rand_frame(&d->frame);
    rand_p3(message_point(d), -2, 2);
    d->num_volumes = ri(0, 3);
    d->volumes[0] = chance(90) ? (CollisionVolume*)at(W_V0) : 0;
    d->volumes[1] = (CollisionVolume*)at(W_V1);
    d->volumes[2] = d->volumes[3] = 0;
    build_volume(at(W_V0), d);
    build_volume(at(W_V1), d);
    d->collide_ground = chance(70);
    d->num_corners = chance(20) ? 0 : ri(1, 8);
    for (int i = 0; i < 8; i++) {
        Corner& c = d->corners[i];
        rand_p3(&c.local_pos, -2, 2);
        rand_p3(&c.ray_origin_local, -0.3f, 0.3f);
        c.stiffness = sp(chance(50) ? rf(1000, 1e5f) : rf(100, 3e4f));
        c.prev_depth = sp(chance(30) ? 0.0f : rf(-0.4f, 0.4f));
        c.ray_origin_world.x = d->frame.pos.x + rf(-2, 2);
        c.ray_origin_world.y = d->frame.pos.y + rf(-2, 2);
        c.ray_origin_world.z = d->frame.pos.z + rf(-2, 2);
        c.first_update = chance(15);
    }
    d->mass = sp(rf(1, 5000));
    rand_p3(&d->inertia, 1, 5000);
    d->inv_mass = 1.0f / d->mass;
    d->inv_inertia.x = 1.0f / d->inertia.x; d->inv_inertia.y = 1.0f / d->inertia.y; d->inv_inertia.z = 1.0f / d->inertia.z;
    memset(&d->inv_inertia_body, 0, 36);
    d->inv_inertia_body.m[0] = d->inv_inertia.x; d->inv_inertia_body.m[4] = d->inv_inertia.y; d->inv_inertia_body.m[8] = d->inv_inertia.z;
    for (int i = 0; i < 9; i++) d->inv_inertia_world.m[i] = sp(i % 4 == 0 ? rf(1e-4f, 1) : rf(-1e-3f, 1e-3f));
    int k = rnd() % 10;
    float vs = k < 2 ? 0.3f : k < 8 ? 40.0f : 400.0f;              // still / moving / past the 300 m/s cap
    rand_p3(&d->velocity, -vs, vs);
    float ws = k < 3 ? 0.001f : k < 8 ? 3.0f : 14.0f;              // tiny step / spinning / past 10 rad/s
    rand_p3(&d->angular_velocity, -ws, ws);
    rand_p3(&d->force, -5e4f, 5e4f);
    rand_p3(&d->torque, -2e4f, 2e4f);
    rand_p3(&d->impulse, -500, 500);
    rand_p3(&d->angular_impulse, -500, 500);
    int wk = rnd() % 10;
    d->impulse_weight = sp(wk < 3 ? 0.0f : wk < 4 ? 1e-8f : rf(0.001f, 20));
    d->num_external_impulses = chance(30) ? 0 : ri(1, 16);
    for (int i = 0; i < 16; i++) {
        ExtImpulse* e = &ext(d)[i];
        rand_p3(&e->impulse, -300, 300);
        rand_p3(&e->point, -2000, 2000);
        e->surface = ri(0, 110);
    }
    uint8_t* tail = o + 0x478;
    if (cls == 1) {
        Obstacle* ob = (Obstacle*)o;
        rand_frame(&ob->spawn_frame);
        ob->last_perturb_time = rf(0, 600);
        ob->awake = chance(50);
    } else if (cls == 2) {
        Wobble* wb = (Wobble*)o;
        rand_frame(&wb->spawn_frame);
        if (chance(40)) wb->frame.rot.m[7] = rf(-0.3f, 0.3f);
        if (chance(10)) wb->frame.rot.m[7] = fbits(chance(50) ? 0xbdb295ea : 0xbdb295e9);
        wb->_4ac = ri(0, 10);
        wb->settle_timer = ri(-2, 25);
        if (chance(15)) wb->settle_timer = 1;
    } else if (cls == 3) {
        Ball* b = (Ball*)o;
        b->last_throw_time = rf(0, 600);
        b->index = ri(0, 15);
        if (chance(30)) b->frame.pos.y = rf(-2500, -1500);
    }
    (void)tail;
}

static void build_data(uint8_t* data) {
    float* f = (float*)data;
    for (int i = 0; i < 32; i++) f[i] = sp(rf(-50, 50));
    *(int32_t*)data = ri(0, 20);
    f[2] = sp(rf(1, 5000));                                   // mass (lb)
    for (int i = 3; i < 6; i++) f[i] = sp(rf(1, 5000));       // moments
    for (int i = 6; i < 9; i++) f[i] = sp(rf(-2000, 2000));   // position
    f[9] = sp(rf(-360, 360));                                 // yaw (degrees) / ball radius (inches)
    for (int i = 10; i < 13; i++) f[i] = sp(rf(0.2f, 5));     // sizes
    *(int32_t*)(data + 0x34) = ri(0, 3);                      // obstacle type / ball slot
    *(int32_t*)(data + 0x38) = chance(80) ? 0 : ri(1, 3);
    for (int i = 15; i < 18; i++) f[i] = sp(rf(0.2f, 20));    // box half extents
}

static void build_checkpoint() {
    CheckPoint* c = (CheckPoint*)at(W_CP);
    memset(c, 0, 0x100);
    c->vtable = (void**)0x004dc350;
    rand_p3(&c->a, -500, 500);
    rand_p3(&c->b, -500, 500);
    if (chance(30)) c->b = c->a;
}

static void build_packet(uint8_t* p, const PhobDyno* d) {
    for (int i = 0; i < 0x40; i++) p[i] = (uint8_t)rnd();
    if (chance(70)) {                                         // a packet the original writes from a frame
        PhobDyno tmp;
        memcpy(&tmp, d, sizeof tmp);
        rand_frame(&tmp.frame);
        rand_p3(&tmp.velocity, -60, 60);
        ((void(__fastcall*)(void*, int, uint8_t*))0x00445940)(&tmp, 0, p);
    }
    if (chance(10)) memset(p, 0, 6);                          // all angles at -2pi
}

// ---- running one call both ways --------------------------------------------------------------------------------
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
struct Stat { const char* name; long calls, changed, fails, fp_fails, faults; };
static Stat g_stats[96];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    g_stats[g_nstats] = {name, 0, 0, 0, 0, 0};
    return g_stats[g_nstats++];
}
static uint8_t g_start[W_SIZE], g_after[W_SIZE];
static Ball* g_balls_start[16];
static Ball* g_balls_after[16];
static Footprint g_fp;
static int g_world;
static unsigned g_pc;

template <typename Run> struct Guard {
    Run* run; bool orig; uint64_t ret;
    static void call(void* c) { Guard* g = (Guard*)c; g->ret = (*g->run)(g->orig); }
};
static int guarded(void (*fn)(void*), void* ctx) {
    __try { fn(ctx); } __except (EXCEPTION_EXECUTE_HANDLER) { _fpreset(); return 1; }
    return 0;
}
static void reset_pass(int pass) {
    g_pass = pass; g_nlog[pass] = 0;
    g_ti = g_tmi = g_ai = g_ri = 0;
    unsigned cw;
    _controlfp_s(&cw, g_pc, _MCW_PC);
}

template <typename Run, typename Fp> static void check(const char* name, Run run, Fp footprint) {
    Stat& st = stat(name);
    st.calls++;
    memcpy(g_start, g_w, W_SIZE);
    memcpy(g_balls_start, k_balls, 64);
    uint32_t heap0 = g_heap_used;
    g_fp.n = 0;
    g_fp.replay_only = 0;
    g_fp.pure = false;
    footprint(g_fp);
    g_pc = chance(12) ? _PC_53 : _PC_24;
    reset_pass(0);
    Guard<Run> go = {&run, true, 0};
    int fo = guarded(&Guard<Run>::call, &go);
    memcpy(g_after, g_w, W_SIZE);
    memcpy(g_balls_after, k_balls, 64);
    uint32_t heap_o = g_heap_used;
    if (memcmp(g_after, g_start, W_SIZE) || memcmp(g_balls_after, g_balls_start, 64)) st.changed++;
    memcpy(g_w, g_start, W_SIZE);
    memcpy(k_balls, g_balls_start, 64);
    g_heap_used = heap0;
    reset_pass(1);
    Guard<Run> gn = {&run, false, 0};
    int fn = guarded(&Guard<Run>::call, &gn);
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    if (fo || fn) st.faults++;
    int nl = g_nlog[0] < LOG_MAX ? g_nlog[0] : LOG_MAX;
    bool bad = fo != fn || (!fo && (go.ret != gn.ret || memcmp(g_after, g_w, W_SIZE) ||
                                    memcmp(g_balls_after, k_balls, 64) || g_nlog[0] != g_nlog[1] ||
                                    memcmp(g_log[0], g_log[1], 4 * nl) || heap_o != g_heap_used));
    if (bad && st.fails++ < 4) {
        printf("MISMATCH %s (world %d, class %d, pc %s)\n", name, g_world, g_class, g_pc == _PC_24 ? "24" : "53");
        if (fo != fn) printf("  original %s, rewrite %s\n", fo ? "faulted" : "ran", fn ? "faulted" : "ran");
        if (go.ret != gn.ret) printf("  return: original %016llx, rewrite %016llx\n", go.ret, gn.ret);
        int shown = 0;
        for (uint32_t i = 0; i < W_SIZE; i += 4)
            if (memcmp(g_after + i, g_w + i, 4) && shown++ < 24) {
                uint32_t rel, a, b, s;
                memcpy(&a, g_after + i, 4); memcpy(&b, g_w + i, 4); memcpy(&s, g_start + i, 4);
                const char* p = part_of(i, &rel);
                printf("  %s+0x%x: start %08x, original %08x (%.9g), rewrite %08x (%.9g)\n", p, rel, s, a, fbits(a), b,
                       fbits(b));
            }
        if (memcmp(g_balls_after, k_balls, 64)) printf("  the ball table differs\n");
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
    // the footprint must cover every byte the original changed
    if (!g_fp.replay_only && !fo) {
        bool missed = false;
        for (uint32_t i = 0; i < W_SIZE && !missed; i++) {
            if (g_after[i] == g_start[i]) continue;
            bool in = false;
            for (int k = 0; k < g_fp.n && !in; k++)
                in = g_w + i >= (uint8_t*)g_fp.r[k].p && g_w + i < (uint8_t*)g_fp.r[k].p + g_fp.r[k].n;
            if (!in) {
                missed = true;
                if (st.fp_fails++ < 4) {
                    uint32_t rel;
                    const char* p = part_of(i, &rel);
                    printf("FOOTPRINT %s (world %d): %s+0x%x changed outside it\n", name, g_world, p, rel);
                }
            }
        }
        if (!missed && memcmp(g_balls_after, g_balls_start, 64)) {
            for (int i = 0; i < 16 && !missed; i++) {
                if (g_balls_after[i] == g_balls_start[i]) continue;
                bool in = false;
                for (int k = 0; k < g_fp.n && !in; k++)
                    in = (uint8_t*)&k_balls[i] >= (uint8_t*)g_fp.r[k].p &&
                         (uint8_t*)&k_balls[i] < (uint8_t*)g_fp.r[k].p + g_fp.r[k].n;
                if (!in && st.fp_fails++ < 4) printf("FOOTPRINT %s (world %d): ball slot %d\n", name, g_world, i);
                missed = !in;
            }
        }
    }
    // go on from the original's result
    memcpy(g_w, g_after, W_SIZE);
    memcpy(k_balls, g_balls_after, 64);
    g_heap_used = heap_o;
}

#define CHECK(FN, ...)                                                                                  \
    do {                                                                                                \
        auto args_ = std::make_tuple(__VA_ARGS__);                                                      \
        make_script();                                                                                  \
        check(VP_CAT(name_, FN),                                                                        \
              [&](bool orig) {                                                                          \
                  auto f = orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN;                    \
                  return std::apply([&](auto... a) { return invoke(f, a...); }, args_);                 \
              },                                                                                        \
              [&](Footprint& fp) { std::apply([&](auto... a) { VP_CAT(fpof_, FN)(fp, a...); }, args_); });\
    } while (0)

// ---- coverage ------------------------------------------------------------------------------------------------
static long c_update_weight, c_update_ext, c_update_vcap, c_update_wcap, c_update_tiny, c_update_ground,
    c_corner_water, c_corner_first, c_throw_plane, c_throw_other, c_wobble_clamp, c_msg_alias, c_vel_alias,
    c_arm_alias, c_ctor_type[4], c_reset_asleep, c_reset_woke;

typedef void*(__fastcall* Ctor2_t)(void*, int, const uint8_t*, void*);
static void prebuild_obj2(int which) {                        // an object for the destructors, by the originals
    uint8_t* o = at(W_OBJ2);
    build_data(at(W_DATA));
    *(int32_t*)(at(W_DATA) + 0x34) = which == 3 ? ri(0, 15) : ri(0, 2);
    make_script();
    memset(g_script.alloc_fail, 0, 4);
    reset_pass(0);
    static const uint32_t ctors[] = {0x004449c0, 0x0043cd90, 0x0043e190, 0x004410f0};
    ((Ctor2_t)(uintptr_t)ctors[which])(o, 0, at(W_DATA), 0);
}

static void run_world() {
    g_special_pct = g_world % 4 == 0 ? 0 : g_world % 4 == 1 ? 1 : 4;
    memset(g_w, 0, W_SIZE);
    for (uint32_t i = W_PA; i < W_HEAP; i += 4) { float x = rf(-100, 100); memcpy(g_w + i, &x, 4); }
    g_heap_used = 0;
    for (int i = 0; i < 16; i++) k_balls[i] = chance(30) ? (Ball*)at(W_OBJ) : chance(50) ? 0 : (Ball*)(uintptr_t)rnd();
    *(void***)at(W_DEITY) = g_deity_vt;
    *k_deity = at(W_DEITY);
    g_class = ri(0, 3);
    build_dyno(at(W_OBJ), g_class);
    build_checkpoint();
    PhobDyno* d = obj();
    // the static object for Wobble's constructor: a frame and a volume at +0x38
    rand_frame((Frame*)at(W_SO));
    *(void**)(at(W_SO) + 0x38) = at(W_V1);
    int steps = ri(4, 16);
    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < 6; i++) rand_p3(p3(i), chance(50) ? -3.0f : -3000.0f, chance(50) ? 3.0f : 3000.0f);
        if (chance(10)) memset(p3(0), 0, 12);
        if (chance(5)) p3(0)->x = 1e-5f;
        int surf = chance(30) ? ri(100, 110) : ri(0, 30);
        float w = sp(chance(20) ? 0.001f : rf(0, 5));
        switch (rnd() % 34) {
        case 0: case 1: case 2: case 3: {
            if (fabsf(d->impulse_weight) > 1.1920929e-7f) { c_update_weight++; if (d->num_external_impulses > 0) c_update_ext++; }
            if (d->velocity.x * d->velocity.x + d->velocity.y * d->velocity.y + d->velocity.z * d->velocity.z > 90000) c_update_vcap++;
            if (d->angular_velocity.x * d->angular_velocity.x + d->angular_velocity.y * d->angular_velocity.y + d->angular_velocity.z * d->angular_velocity.z > 100) c_update_wcap++;
            if (fabsf(d->angular_velocity.x) < 0.005f && fabsf(d->angular_velocity.y) < 0.005f) c_update_tiny++;
            if (d->num_volumes > 0 && d->volumes[0] && d->collide_ground) c_update_ground++;
            CHECK(PhobDyno_Update, d, 0);
            break;
        }
        case 4: case 5: {
            int k = ri(0, 7);
            if (d->corners[k].first_update) c_corner_first++;
            CHECK(Corner_Update, &d->corners[k], 0, d);
            if (g_script.terrain[0].hit && g_script.terrain[0].surface == 14) c_corner_water++;
            break;
        }
        case 6: CHECK(PhobDyno_ApplyForce, d, 0, p3(0), chance(20) ? p3(0) : p3(1)); break;
        case 7: CHECK(PhobDyno_ApplyTorque, d, 0, p3(0)); break;
        case 8: CHECK(PhobDyno_ApplyImpulse, d, 0, p3(0), p3(1), w); break;
        case 9: CHECK(PhobDyno_QueueExternalImpulse, d, 0, p3(0), p3(1), surf, w); break;
        case 10: {
            P3* out = chance(15) ? &d->angular_velocity : chance(20) ? p3(1) : p3(2);
            if (out == &d->angular_velocity) c_vel_alias++;
            CHECK(PhobDyno_GetPointVelocity, d, 0, out, p3(1));
            break;
        }
        case 11: CHECK(PhobDyno_GetPointMoment, d, 0, chance(20) ? p3(1) : p3(2), p3(1)); break;
        case 12: if (chance(50)) CHECK(PhobDyno_reset, d, 0); else CHECK(PhobDyno_Reset, d, 0); break;
        case 13: CHECK(PhobDyno_ApplyExternalForce, d, 0, p3(0), p3(1), surf); break;
        case 14: {
            build_packet(at(W_PA), d);
            build_packet(at(W_PB), d);
            float t = sp(chance(20) ? (float)ri(0, 1) : rf(0, 1));
            if (chance(50)) CHECK(PhobDyno_UpdateReplay, d, 0, (const uint8_t*)at(W_PA), (const uint8_t*)at(W_PB), t);
            else CHECK(PhobRoot_UpdateReplay, (PhobRoot*)d, 0, (const uint8_t*)at(W_PA), (const uint8_t*)at(W_PB), t);
            break;
        }
        case 15:
            if (chance(50)) CHECK(PhobDyno_MakeReplayPacket, d, 0, at(W_PK));
            else CHECK(PhobRoot_MakeReplayPacket, (PhobRoot*)d, 0, at(W_PK));
            break;
        case 16: {
            bool al = chance(25);
            if (al) c_arm_alias++;
            CHECK(PhobRoot_GetArmPosition, (PhobRoot*)d, 0, al ? p3(0) : p3(2), p3(0));
            break;
        }
        case 17: {
            rand_frame((Frame*)at(W_FA));
            rand_frame((Frame*)at(W_FB));
            Frame* o = chance(20) ? (Frame*)at(W_FA) : (Frame*)at(W_FO);
            CHECK(frame_combine_rw, o, (const Frame*)at(W_FA), (const Frame*)at(W_FB), sp(rf(0, 1)));
            break;
        }
        case 18: CHECK(VectorNormalize_rw, p3(0)); break;
        case 19:
            switch (rnd() % 11) {
            case 0: CHECK(PhobRoot_GetReplayPacketSize, (PhobRoot*)d, 0); break;
            case 1: CHECK(PhobRoot_GetDyno, (PhobRoot*)d, 0); break;
            case 2: CHECK(PhobRoot_IsDynamic, (PhobRoot*)d, 0); break;
            case 3: CHECK(PhobStatic_GetMessageSize, (PhobRoot*)d, 0); break;
            case 4: CHECK(PhobRoot_GetMessageSize, (PhobRoot*)d, 0); break;
            case 5: CHECK(PhobRoot_IsSolid, (PhobRoot*)d, 0); break;
            case 6: CHECK(PhobDyno_GetReplayPacketSize, (PhobRoot*)d, 0); break;
            case 7: CHECK(Wobble_GetReplayPacketSize, (PhobRoot*)d, 0); break;
            case 8: CHECK(PhobRoot_Reset, (PhobRoot*)d, 0); break;
            case 9: CHECK(ExternalImpulse_ctor, &d->external_impulses[ri(0, 15)], 0); break;
            default: CHECK(GetBall_rw, ri(0, 15)); break;
            }
            break;
        case 20: {
            bool al = chance(30);
            uint8_t* buf = al ? (uint8_t*)d - d->_008 : at(W_MSG) + ri(0, 0x80);
            if (al) c_msg_alias++;
            CHECK(PhobRoot_GetMessage, (PhobRoot*)d, 0, buf);
            break;
        }
        case 21: {
            int k = ri(0, 7);
            if (chance(30)) CHECK(Corner_ctor, &d->corners[k], 0);
            else CHECK(Corner_Setup, &d->corners[k], 0, p3(0), sp(rf(10, 1e5f)), chance(20) ? p3(0) : p3(1));
            break;
        }
        case 22: case 23: case 24: case 25:                     // the class's own functions
            if (g_class == 1) {
                Obstacle* ob = (Obstacle*)d;
                switch (rnd() % 3) {
                case 0: {
                    bool asleep = !ob->awake;
                    CHECK(Obstacle_Reset, ob, 0);
                    if (asleep) { c_reset_asleep++; if (ob->awake) c_reset_woke++; }   // (the original's result)
                    break;
                }
                case 1: CHECK(Obstacle_Perturb, ob, 0); break;
                default: CHECK(Obstacle_ApplyExternalForce, ob, 0, p3(0), p3(1), surf); break;
                }
            } else if (g_class == 2) {
                Wobble* wb = (Wobble*)d;
                if (chance(25)) CHECK(Wobble_Reset, wb, 0);
                else {
                    if (wb->settle_timer > 0 || wb->num_external_impulses > 0) c_wobble_clamp++;
                    CHECK(Wobble_Update, wb, 0);
                }
            } else if (g_class == 3) {
                Ball* b = (Ball*)d;
                switch (rnd() % 3) {
                case 0: CHECK(Ball_Update, b, 0); break;
                case 1: CHECK(Ball_Reset, b, 0); break;
                default: {
                    rand_frame((Frame*)at(W_FA));
                    float now = rf(0, 600);
                    b->last_throw_time = chance(70) ? now - rf(2.1f, 50) : now - rf(0, 2);
                    float t = chance(70) ? now - rf(0, 0.12f) : now + rf(-1, 1);
                    g_force_time = true;                        // make_script (in CHECK) sets the clock to it
                    g_forced_time = now;
                    if (g_vrmod) {                              // cooldown 0.05-5 s, speed x0.25-15, ahead -30..30 m, up -2..10 m
                        vrmod_put32(0x004dc39c, ubits(rf(0.05f, 5.0f)));
                        vrmod_put32(0x004dc3a4, ubits(31.11111f * rf(0.25f, 15.0f)));
                        vrmod_put32(0x004dc3a8, ubits(rf(-30.0f, 30.0f)));
                        vrmod_put32(0x004dc3ac, ubits(rf(-2.0f, 10.0f)));
                        if (chance(5)) vrmod_put32(0x004dc3a4 + 4 * (rnd() % 3), rnd());   // anything at all
                    }
                    CHECK(Ball_Throw, b, 0, (const Frame*)at(W_FA), p3(1), sp(t));
                    g_force_time = false;
                    if (g_script.plane) c_throw_plane++; else c_throw_other++;
                    break;
                }
                }
            } else {
                CHECK(PhobDyno_Update, d, 0);
            }
            break;
        case 26: {
            CheckPoint* c = (CheckPoint*)at(W_CP);
            P3* p = p3(0);
            P3* q = p3(1);
            if (chance(60)) {                                   // a move across the gate
                float t = rf(-0.2f, 1.2f);
                float mx = c->a.x + (c->b.x - c->a.x) * t, mz = c->a.z + (c->b.z - c->a.z) * t;
                float nx = -(c->b.z - c->a.z), nz = c->b.x - c->a.x;
                float s1 = rf(-0.5f, 0.5f), s2 = rf(-0.5f, 0.5f);
                p->x = mx + nx * s1; p->z = mz + nz * s1;
                q->x = mx + nx * s2; q->z = mz + nz * s2;
            }
            CHECK(CheckPoint_HasCrossed, c, 0, (const P3*)p, (const P3*)q);
            break;
        }
        case 27:
            switch (rnd() % 2) {
            case 0: CHECK(register_ball_rw, (Ball*)d, ri(0, 15)); break;
            default: CHECK(unregister_ball_rw, chance(70) ? (Ball*)d : k_balls[ri(0, 15)]); break;
            }
            break;
        case 28: case 29: case 30: {                            // constructors, on the second object
            build_data(at(W_DATA));
            uint8_t* o = at(W_OBJ2);
            for (int i = 0; i < 0x500; i += 4) *(uint32_t*)(o + i) = rnd();
            void* arg = (void*)(uintptr_t)ri(0, 0x200);
            const uint8_t* data = at(W_DATA);
            switch (rnd() % 8) {
            case 0: CHECK(PhobRoot_ctor, (PhobRoot*)o, 0, data, arg); break;
            case 1: CHECK(PhobDyno_ctor, (PhobDyno*)o, 0, data, arg); break;
            case 2: case 3: {
                int ty = *(const int32_t*)(data + 0x34);
                c_ctor_type[ty & 3]++;
                CHECK(Obstacle_ctor, (Obstacle*)o, 0, data, arg);
                break;
            }
            case 4: CHECK(Wobble_ctor, (Wobble*)o, 0, data, arg); break;
            case 5: *(int32_t*)(at(W_DATA) + 0x34) = ri(0, 15); CHECK(Ball_ctor, (Ball*)o, 0, data, arg); break;
            case 6: CHECK(PhobStatic_ctor, (PhobRoot*)o, 0, data, arg); break;
            default: CHECK(CheckPoint_ctor, (CheckPoint*)at(W_CP), 0, data, arg); build_checkpoint(); break;
            }
            break;
        }
        case 31: case 32: {                                     // destructors, on an object the originals built
            int which = ri(0, 3);
            prebuild_obj2(which);
            PhobRoot* o = (PhobRoot*)at(W_OBJ2);
            unsigned flags = ri(0, 3);
            switch (which == 3 ? rnd() % 3 : rnd() % 7) {
            case 0: if (which == 3) CHECK(Ball_dtor, (Ball*)o, 0); else CHECK(PhobRoot_dtor, o, 0); break;
            case 1: if (which == 3) CHECK(Ball_scalar_deleting_dtor, o, 0, flags); else CHECK(PhobRoot_vector_deleting_dtor, o, 0, flags); break;
            case 2: CHECK(PhobStatic_vector_deleting_dtor, o, 0, flags); break;
            case 3: CHECK(PhobDyno_scalar_deleting_dtor, o, 0, flags); break;
            case 4: CHECK(Obstacle_scalar_deleting_dtor, o, 0, flags); break;
            case 5: CHECK(Wobble_vector_deleting_dtor, o, 0, flags); break;
            default: CHECK(CheckPoint_scalar_deleting_dtor, o, 0, flags); break;
            }
            break;
        }
        default: {                                              // the rest of the class's virtuals
            if (g_class == 1) CHECK(Obstacle_ApplyExternalForce, (Obstacle*)d, 0, p3(0), p3(1), surf);
            else if (g_class == 2) CHECK(Wobble_Update, (Wobble*)d, 0);
            else if (g_class == 3) CHECK(Ball_Update, (Ball*)d, 0);
            else CHECK(PhobDyno_QueueExternalImpulse, d, 0, p3(0), p3(1), surf, w);
            break;
        }
        }
        // a message written at the object itself puts the tick count over its vtable (as in the game):
        // rebuild it then, and now and again anyway
        if (chance(10) || ((uintptr_t)d->vtable != k_vt_orig[g_class] && d->vtable != g_vt_copy[g_class]))
            build_dyno(at(W_OBJ), g_class);
    }
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 20000;
    if (argc > 2) g_state = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_dyno.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    g_vrmod = getenv("VP_VRMOD") && atoi(getenv("VP_VRMOD"));
    if (g_vrmod) puts("vrmod's hornball: Ball::Throw's tunings random");
    patch_jmp(0x00465be0, (void*)&stub_terrain);
    patch_jmp(0x00465ca0, (void*)&stub_terrain_sphere);
    patch_jmp(0x0043c710, (void*)&stub_collide);
    patch_jmp(0x0043c950, (void*)&stub_collide_water);
    patch_jmp(0x0042bc80, (void*)&stub_time);
    patch_jmp(0x004140e0, (void*)&stub_alloc);
    patch_jmp(0x00414390, (void*)&stub_delete);
    patch_jmp(0x00411150, (void*)&stub_logreport);
    patch_jmp(0x00426d80, (void*)&stub_find_static);
    patch_jmp(0x0040d5a0, (void*)&stub_car_index);
    patch_jmp(0x00406910, (void*)&stub_car_name);
#ifdef VP_TEST_FIXES
    // vrmod's obstacle wake: Obstacle::Reset's `ret; mov edi, edi` NOPed, so it falls through into Perturb
    static const uint8_t k_stock[3] = {0xc3, 0x8b, 0xff}, k_nops[3] = {0x90, 0x90, 0x90};
    if (memcmp((void*)0x0043d3dd, k_stock, 3)) { printf("Obstacle::Reset isn't stock\n"); return 2; }
    memcpy((void*)0x0043d3dd, k_nops, 3);
    printf("fix build: against the original with vrmod's obstacle-wake patch\n");
#endif
    for (int i = 0; i < 16; i++) g_deity_vt[i] = (void*)&stub_deity;
    for (int c = 0; c < 4; c++) {
        memcpy(g_vt_copy[c], (void*)(uintptr_t)k_vt_orig[c], 15 * 4);
        g_vt_copy[c][14] = (void*)&stub_resolve;                // +0x38 ResolveExternalImpulse
    }
    g_w = (uint8_t*)VirtualAlloc(0, W_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    for (g_world = 0; g_world < worlds; g_world++) run_world();
    int failed = 0;
    long total = 0;
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("  %-44s %8ld calls, %8ld changed, %6ld faulted: %s%s\n", st.name, st.calls, st.changed, st.faults,
               st.fails ? "DIFFERS" : "identical", st.fp_fails ? "  FOOTPRINT MISSES" : "");
        failed += st.fails || st.fp_fails;
        total += st.calls;
    }
    printf("coverage: Update with impulses %ld (queued %ld), speed cap %ld, spin cap %ld, tiny spin %ld, ground volume %ld;\n"
           "  corner first update %ld, water %ld; throw plane %ld / other %ld; wobble running %ld;\n"
           "  aliases: message %ld, point velocity %ld, arm %ld; obstacle types 0-3: %ld %ld %ld %ld\n",
           c_update_weight, c_update_ext, c_update_vcap, c_update_wcap, c_update_tiny, c_update_ground, c_corner_first,
           c_corner_water, c_throw_plane, c_throw_other, c_wobble_clamp, c_msg_alias, c_vel_alias, c_arm_alias,
           c_ctor_type[0], c_ctor_type[1], c_ctor_type[2], c_ctor_type[3]);
    printf("  Obstacle::Reset of a sleeping obstacle: %ld, awake after it: %ld\n", c_reset_asleep, c_reset_woke);
#ifdef VP_TEST_FIXES
    if (c_reset_woke != c_reset_asleep || !c_reset_asleep) { printf("FIX: a reset left an obstacle asleep\n"); failed++; }
#else
    if (c_reset_woke) { printf("FAITHFUL: a reset woke an obstacle\n"); failed++; }
#endif
    printf("%d worlds, %ld checks; %d of %d functions differ; %d unknown footprint classes\n", worlds, total, failed,
           g_nstats, g_fp_unknown);
    return failed || g_fp_unknown ? 1 : 0;
}
