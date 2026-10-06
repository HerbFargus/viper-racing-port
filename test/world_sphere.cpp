// world_sphere.cpp -- SphereVolume::CollideGround and CubeVolume::CollideGround (hook/phys_sphere.cpp) against the
// originals, on random worlds outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_sphere.cpp
//        /Fo<dir>\ /Fe<dir>\world_sphere.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_sphere.exe [worlds] [seed]
//   the fixes (docs/PORTING.md, "Fixes"): the same with /DFIX_TESTS (or /DVP_TEST_FIXES) -- the rewrites as the game
//   builds them (every random world must still match); then balls that barely touch the ground with no tangential
//   velocity, run with the physics thread's x87 exceptions (zero-divide and overflow unmasked): the original faults
//   on some, the fixed rewrite on none, and matches the original wherever the original doesn't fault.
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. A world is a PhobDyno owner (PhobDyno's vtable: its
// ApplyExternalForce, GetPointVelocity, QueueExternalImpulse run as the game's code) and a Sphere or Cube volume on
// it built by the ORIGINAL constructor. The real TerrainGetSphereIntersection runs, over a stubbed TerrainGetHeight
// that answers from a script: a terrain point placed so the centre's height over the plane is a chosen s (around the
// radius: misses, hits, barely touching), a normal (straight up, tilted, a hair off unit), a surface (water too).
// Collide and CollideWater are logging stubs; PhysicsGetTime reads physics_tick, set at random. Each check runs the
// original, restores, runs the rewrite, and compares the owner, the volume, the stubs' log and any fault.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif
#if defined(FIX_TESTS) && !defined(VP_TEST_FIXES)
#define VP_TEST_FIXES
#endif
#ifndef VP_TEST_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#endif
#include "../hook/port.h"
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN) static const uint32_t VP_CAT(addr_, NEW) = V10;

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

#include "../hook/phys_sphere.cpp"

// ---- random values -------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float rf(float lo, float hi) { return lo + (hi - lo) * uni(); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t ubits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// ---- the original, loaded at 0x400000 ------------------------------------------------------------------------------
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
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize);
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

// ---- the world, the script, the log ----------------------------------------------------------------------------
struct World {
    alignas(16) uint8_t owner[0x480];
    uint8_t vol[0x80];
    int32_t tick;                                     // physics_tick
};
static World W, g_start, g_after;
static int32_t* const k_tick = (int32_t*)0x0052161c;
struct Script { uint8_t hit; float hx, hy, hz; P3 n; int32_t surface; };
static Script g_script;
enum { LOG_MAX = 256 };
static uint32_t g_log[2][LOG_MAX];
static int g_nlog[2], g_pass;
static void log_put(uint32_t v) { if (g_nlog[g_pass] < LOG_MAX) g_log[g_pass][g_nlog[g_pass]] = v; g_nlog[g_pass]++; }
static void log_p3(const P3* p) { log_put(ubits(p->x)); log_put(ubits(p->y)); log_put(ubits(p->z)); }

static uint8_t __cdecl stub_height(float x, float z, P3* h, P3* n, int32_t* surface) {
    log_put(0x7e000001); log_put(ubits(x)); log_put(ubits(z));
    if (!g_script.hit) return 0;
    h->x = g_script.hx; h->y = g_script.hy; h->z = g_script.hz;
    *n = g_script.n;
    *surface = g_script.surface;
    return 1;
}
static void __cdecl stub_collide(void* a, void* b, const P3* p, const P3* i, const P3* n) {
    log_put(0xc0000001); log_put((uint32_t)(uintptr_t)a); log_put((uint32_t)(uintptr_t)b); log_p3(p); log_p3(i); log_p3(n);
}
static void __cdecl stub_collide_water(void* v, P3* p, const P3* vel) {
    log_put(0xc0000002); log_put((uint32_t)(uintptr_t)v); log_p3(p); log_p3(vel);
}

typedef void*(__fastcall* VolCtor_t)(void*, int, const Frame*, float a, float b, const P3* c, float r, PhobDyno*);
static bool g_cube;
// kind: 0 random, 1 barely touching (s within a few ulps of r) at rest or moving along N
static void build(int kind) {
    memset(&W, 0, sizeof W);
    PhobDyno* d = (PhobDyno*)W.owner;
    d->vtable = (void**)0x004dc690;
    double yaw = rf(-3.1416f, 3.1416f), pitch = rf(-0.3f, 0.3f);
    M3& R = d->frame.rot;
    R.m[0] = (float)cos(yaw); R.m[2] = (float)-sin(yaw); R.m[6] = (float)sin(yaw); R.m[8] = (float)cos(yaw); R.m[4] = 1;
    if (chance(50)) { R.m[4] = (float)cos(pitch); R.m[5] = (float)sin(pitch); R.m[7] = (float)-sin(pitch); }
    d->frame.pos = {rf(-2000, 2000), rf(-50, 300), rf(-2000, 2000)};
    d->mass = rf(1, 3000);
    for (int i = 0; i < 9; i++) d->inv_inertia_world.m[i] = i % 4 == 0 ? rf(1e-4f, 0.5f) : rf(-1e-3f, 1e-3f);
    const bool still = kind == 1 ? chance(60) : chance(15);
    if (!still) {
        d->velocity = {rf(-40, 40), rf(-10, 10), rf(-40, 40)};
        d->angular_velocity = {rf(-3, 3), rf(-3, 3), rf(-3, 3)};
    }
    g_cube = chance(30);
    P3 c = {rf(-1.5f, 1.5f), rf(-1, 1), rf(-2, 2)};
    if (kind == 1 && chance(50)) c = {0, 0, 0};
    const float r = rf(0.1f, 2.0f);
    ((VolCtor_t)(uintptr_t)(g_cube ? 0x00432930 : 0x00432340))(W.vol, 0, &d->frame, rf(1e3f, 2e5f), rf(0.05f, 1.0f), &c, r, d);
    SphereVolume* v = (SphereVolume*)W.vol;
    v->surface_type = chance(70) ? 100 : (int)(rnd() % 20);
    v->_38 = (uint8_t)chance(20);                                   // floating
    v->_3c = ubits(d->frame.pos.y + rf(-3, 3));
    ((void(__fastcall*)(void*, int))VFN(v, 4, void))(v, 0);        // Update: the world centre
    W.tick = (int32_t)(rnd() % 100000);
    // the terrain under the centre: a normal, and a point so the centre is s over the plane along it
    Script& s = g_script;
    double nx, ny, nz;
    int nk = rnd() % 10;
    if (nk < 4 || kind == 1) { nx = 0; ny = 1; nz = 0; }
    else if (nk < 8) { nx = rf(-0.3f, 0.3f); nz = rf(-0.3f, 0.3f); ny = 1; }
    else { nx = rf(-1, 1); nz = rf(-1, 1); ny = rf(0.1f, 1); }
    double l = sqrt(nx * nx + ny * ny + nz * nz);
    s.n = {(float)(nx / l), (float)(ny / l), (float)(nz / l)};
    if (kind == 1 && chance(50)) {                                  // a tilted plane a hair off unit length
        s.n = {rf(-0.3f, 0.3f), 1, rf(-0.3f, 0.3f)};
        double m = sqrt((double)s.n.x * s.n.x + (double)s.n.y * s.n.y + (double)s.n.z * s.n.z);
        s.n = {(float)(s.n.x / m), (float)(s.n.y / m), (float)(s.n.z / m)};
        uint32_t u = ubits(s.n.y) + (rnd() % 3);                    // up to 2 ulps long
        s.n.y = fbits(u);
    }
    const P3 C = v->world_center;
    double target = kind == 1 ? r * (1.0 - (double)(rnd() % 64) * 6e-8) - (double)rf(0, 2e-5f) * (chance(50) ? 1 : 0)
                              : r * (double)rf(-1.3f, 1.3f);
    s.hx = C.x + rf(-0.05f, 0.05f);
    s.hz = C.z + rf(-0.05f, 0.05f);
    // (C - H).N = target: H.y from the other two components
    s.hy = (float)(C.y - (target - ((double)C.z - s.hz) * s.n.z - ((double)C.x - s.hx) * s.n.x) / s.n.y);
    s.hit = kind == 1 || chance(90);
    static const int surfaces[] = {0, 0, 0, 1, 5, 10, 14, 14, 20, 100};
    s.surface = kind == 1 ? (int)(rnd() % 10) : surfaces[rnd() % 10];
    if (kind == 1 && s.surface == 14) s.surface = 3;
    if (kind == 1 && !still) {                                      // moving straight along N: no tangential velocity
        float k = rf(-3, 3);
        d->velocity = {s.n.x * k, s.n.y * k, s.n.z * k};
        if (s.n.x != 0 || s.n.z != 0) d->velocity = {0, 0, 0};      // (only straight up is exact)
        d->angular_velocity = {0, 0, 0};
    }
}

static unsigned g_code;
static int filt(EXCEPTION_POINTERS* e) { g_code = e->ExceptionRecord->ExceptionCode; return EXCEPTION_EXECUTE_HANDLER; }
static bool g_physics_masks;                                        // zero-divide and overflow unmasked, as the physics thread
static int run(bool orig) {
    g_pass = orig ? 0 : 1;
    g_nlog[g_pass] = 0;
    *k_tick = W.tick;
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    if (g_physics_masks) _controlfp_s(&cw, _EM_INVALID | _EM_UNDERFLOW | _EM_INEXACT | _EM_DENORMAL, _MCW_EM);
    int fault = 0;
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] {
            void* v = W.vol;
            if (orig) ((void(__fastcall*)(void*, int))(uintptr_t)(g_cube ? 0x00432980 : 0x004324d0))(v, 0);
            else if (g_cube) CubeVolume_CollideGround((CubeVolume*)v, 0);
            else SphereVolume_CollideGround((SphereVolume*)v, 0);
            volatile float z = 1.0f;
            z = z + 0.0f;                                           // a pending x87 exception is raised here
        }, filt)) { fault = 1; _fpreset(); }
#else
    __try {
        void* v = W.vol;
        if (orig) ((void(__fastcall*)(void*, int))(uintptr_t)(g_cube ? 0x00432980 : 0x004324d0))(v, 0);
        else if (g_cube) CubeVolume_CollideGround((CubeVolume*)v, 0);
        else SphereVolume_CollideGround((SphereVolume*)v, 0);
        volatile float z = 1.0f;
        z = z + 0.0f;                                               // a pending x87 exception is raised here
    } __except (filt(GetExceptionInformation())) { fault = 1; _fpreset(); }
#endif
    _controlfp_s(&cw, _CW_DEFAULT, 0xffffffff);
    _clearfp();
    return fault;
}
// original, then rewrite, from the same world; 0 same, 1 differ. *fo: the original faulted
static int check(int* fo_out) {
    memcpy(&g_start, &W, sizeof W);
    int fo = run(true);
    memcpy(&g_after, &W, sizeof W);
    memcpy(&W, &g_start, sizeof W);
    int fn = run(false);
    if (fo_out) *fo_out = fo;
    int nl = g_nlog[0] < LOG_MAX ? g_nlog[0] : LOG_MAX;
    return fo != fn || (!fo && (memcmp(&g_after, &W, sizeof W) || g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], 4 * nl)));
}
static void show(const char* what, int it) {
    printf("  MISMATCH %s (world %d, %s)\n", what, it, g_cube ? "cube" : "sphere");
    const uint8_t* a = (const uint8_t*)&g_after;
    const uint8_t* b = (const uint8_t*)&W;
    for (uint32_t i = 0, k = 0; i < sizeof W && k < 8; i += 4)
        if (memcmp(a + i, b + i, 4)) {
            uint32_t x, y;
            memcpy(&x, a + i, 4); memcpy(&y, b + i, 4);
            printf("    %s+0x%x original %08x (%.9g), rewrite %08x (%.9g)\n", i < 0x480 ? "owner" : "volume", i < 0x480 ? i : i - 0x480,
                   x, fbits(x), y, fbits(y));
            k++;
        }
    if (g_nlog[0] != g_nlog[1]) printf("    stub logs %d / %d entries\n", g_nlog[0], g_nlog[1]);
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 200000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_sphere.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    patch_jmp(0x00465c40, (void*)&stub_height);
    patch_jmp(0x0043c710, (void*)&stub_collide);
    patch_jmp(0x0043c950, (void*)&stub_collide_water);
    int failed = 0;
    // the random worlds, every exception masked (as the harnesses run the originals): bit for bit
    {
        long n[2] = {0, 0}, bad[2] = {0, 0}, faults = 0, hits = 0, fixed_cases = 0;
        (void)fixed_cases;
        for (int it = 0; it < worlds; it++) {
            build(chance(10) ? 1 : 0);
            int fo;
            int b = check(&fo);
            n[g_cube]++;
            faults += fo;
            hits += g_script.hit;
#ifdef VP_TEST_FIXES
            if (b) {                                                // the fix's case: where the original zero-divides
                World rw;                                           // on the physics thread (masked here, it makes NaNs)
                memcpy(&rw, &W, sizeof W);
                memcpy(&W, &g_start, sizeof W);
                g_physics_masks = true;
                const int f = run(true);
                g_physics_masks = false;
                const bool zdiv = f && g_code == STATUS_FLOAT_DIVIDE_BY_ZERO;
                memcpy(&W, &rw, sizeof W);
                if (zdiv) { fixed_cases++; b = 0; }
            }
#endif
            if (b && bad[g_cube]++ < 4) show("random", it);
        }
#ifdef VP_TEST_FIXES
        printf("  (fix build: %ld random worlds differ where the original zero-divides on the physics thread)\n", fixed_cases);
#endif
        printf("  %-32s %8ld worlds: %s\n", "SphereVolume::CollideGround", n[0], bad[0] ? "DIFFERS" : "identical");
        printf("  %-32s %8ld worlds: %s\n", "CubeVolume::CollideGround", n[1], bad[1] ? "DIFFERS" : "identical");
        printf("  (terrain scripted to hit %ld times; %ld faulted both ways)\n", hits, faults);
        failed += bad[0] != 0 || bad[1] != 0;
    }
    // barely touching, no tangential velocity, with the physics thread's exceptions: the zero divide
    {
        g_physics_masks = true;
        long n = 0, orig_faults = 0, new_faults = 0, bad = 0, cube_faults = 0, cube_n = 0;
        for (int it = 0; it < worlds; it++) {
            build(1);
            int fo;
            memcpy(&g_start, &W, sizeof W);
            const int b = check(&fo);
            memcpy(&W, &g_start, sizeof W);
            const int fn = run(false);
            if (g_cube) { cube_n++; cube_faults += fo; continue; }
            n++;
            orig_faults += fo;
            new_faults += fn;
#ifdef VP_TEST_FIXES
            if (!fo && b && bad++ < 4) show("barely touching (the original ran)", it);
#else
            if (b && bad++ < 4) show("barely touching", it);
#endif
        }
        g_physics_masks = false;
        printf("  barely touching, no tangential velocity, physics-thread exceptions: spheres %ld (the original faulted %ld,"
               " the rewrite %ld), cubes %ld (the original faulted %ld); %ld differ\n",
               n, orig_faults, new_faults, cube_n, cube_faults, bad);
#ifdef VP_TEST_FIXES
        failed += bad != 0 || new_faults != 0 || orig_faults == 0 || cube_faults != 0;
#else
        failed += bad != 0 || cube_faults != 0;
#endif
    }
    printf("%d worlds; %s\n", worlds, failed ? "FAILED" : "all as expected");
    return failed ? 1 : 0;
}
