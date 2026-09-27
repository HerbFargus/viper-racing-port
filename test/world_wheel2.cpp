// world_wheel2.cpp -- the wheel.obj rewrites (hook/phys_wheel.cpp, all but Wheel::Update) against the originals,
// on random worlds, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC /DVP_FUZZ test\world_wheel2.cpp
//        /Fo<outdir>\ /Fe<outdir>\world_wheel2.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_wheel2.exe [iterations] [name-filter] [seed]
//   the fixes (docs/PORTING.md, "Fixes"): the same with /DFIX_TESTS (or /DVP_TEST_FIXES) -- the rewrites as the game builds them.
//   Wheel::Setup with a 31-character car name then must match the original on everything but the tyre width
//   passed to TireCreate, which must be the one the original computes before its name buffer overruns it (the
//   original's width with the name one character shorter).
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does, and includes the rewrite file itself so its
// static functions can be called directly. A World (a Car with its four Wheels, a CarData, a hub vertex, a
// Tire and a TireSound) is built in memory: wheels constructed by the ORIGINAL constructor, then given random
// state. Each check snapshots the world, runs the original, keeps the result, restores the snapshot, runs the
// rewrite and compares the whole world, the return value, and a log of the calls the function makes to the
// stubbed callees: TireCreate, TireSound::Create, ResourceExists, operator delete and the TireSound's own
// vtable (patched with jumps to recorders here). Damper::Setup, GetLoadFluctuation, PhobRoot::GetArmPosition,
// sprintf, __ftol and update_wheel_position (called by UpdateReplay) run as the game's own code;
// PhysicsGetTemperature reads its global, which each world sets at random.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(FIX_TESTS) && !defined(VP_TEST_FIXES)
#define VP_TEST_FIXES           // (the flag the other harnesses use)
#endif
#ifndef VP_TEST_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#endif
#include "../hook/phys_wheel.cpp"

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
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
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

// a value in a plausible range for a car file, now and then anything at all
static float plausible(float lo, float hi) {
    uint32_t k = fuzz_rand() % 100;
    if (k < 85) return lo + (hi - lo) * ((float)(fuzz_rand() & 0xffffff) / 16777216.0f);
    return fuzz_float();
}

// ---- the world ------------------------------------------------------------------------------------------------
struct FakeTireSound { void** vtable; uint32_t pad[15]; };
struct World {
    alignas(16) Car car;
    uint8_t cd[0x200];                 // CarData
    P3 hub;
    Tire tire;
    FakeTireSound ts;
    WheelMessage msg;
    WheelReplayPacket p0, p1, out;
    uint8_t _pad[1];
};
static World g_world, g_snap, g_after;
static float* const g_temperature = (float*)0x004ecf84;    // PhysicsGetTemperature's global
static float g_temp_snap;

// ---- the callees' log ---------------------------------------------------------------------------------------
enum { LOG_MAX = 256 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void log_word(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n++] = w; }
static void log_str(const char* s) {
    size_t len = strlen(s);
    log_word((uint32_t)len);
    for (size_t i = 0; i < len; i += 4) { uint32_t w = 0; memcpy(&w, s + i, len - i < 4 ? len - i : 4); log_word(w); }
}
static uint8_t g_resource_exists;

static Tire* __cdecl stub_TireCreate(uint32_t w, uint32_t a, uint32_t d, const char* file) {
    log_word('TIRE'); log_word(w); log_word(a); log_word(d); log_str(file);
    return &g_world.tire;
}
static TireSound* __cdecl stub_TireSoundCreate(int car_index, int wheel, Wheel* self, Car* car) {
    log_word('TSND'); log_word(car_index); log_word(wheel); log_word((uint32_t)self); log_word((uint32_t)car);
    return (TireSound*)&g_world.ts;
}
static uint8_t __cdecl stub_ResourceExists(const char* file) {
    log_word('RSRC'); log_str(file);
    return g_resource_exists;
}
static void __cdecl stub_delete(void* p) { log_word('DELE'); log_word((uint32_t)p); }
static void* __fastcall stub_ts_dtor(void* self, int, unsigned flags) {
    log_word('TSDT'); log_word((uint32_t)self); log_word(flags);
    return self;
}
static void __fastcall stub_ts_update(void* self, int) { log_word('TSUP'); log_word((uint32_t)self); }
static void* g_ts_vtable[4] = {(void*)&stub_ts_dtor, (void*)&stub_ts_update, 0, 0};

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

// ---- random worlds ----------------------------------------------------------------------------------------------
typedef Wheel*(__fastcall* WheelCtor_t)(Wheel*, Edx);
static const WheelCtor_t orig_ctor = (WheelCtor_t)0x00447ed0;

static void random_bytes(void* p, size_t n) {
    uint8_t* b = (uint8_t*)p;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)fuzz_rand();
}
static void random_floats(void* p, size_t n) {
    for (size_t i = 0; i + 4 <= n; i += 4) { float f = fuzz_float(); memcpy((uint8_t*)p + i, &f, 4); }
}

// a constructed wheel with random state; pointers valid
static void random_wheel(Wheel* w) {
    orig_ctor(w, 0);
    void** vt = w->vtable;
    random_floats(w, sizeof(Wheel));
    w->vtable = vt;
    w->hub_vertex = &g_world.hub;
    w->tire = (fuzz_rand() & 7) ? &g_world.tire : 0;
    w->tire_sound = (fuzz_rand() & 7) ? (TireSound*)&g_world.ts : 0;
    uint32_t b = fuzz_rand();
    w->broken = (b & 3) == 0 ? 0 : (b & 3) == 1 ? 1 : (uint8_t)(b >> 8);
    w->fx_flags = (uint8_t)(b >> 16);
    w->surface_class = (uint8_t)(b >> 24);
    w->on_ground = (uint8_t)fuzz_rand();
}

static void random_world() {
    World& W = g_world;
    random_floats(&W, sizeof W);
    Car& car = W.car;
    car.vtable = (void**)0x004dbfd0;
    car.car_index = (int)(fuzz_rand() % 8);
    // a name of 0..31 characters (31 overruns the tyre-file buffer by its terminator: see Wheel::Setup)
    int len = (fuzz_rand() & 3) == 0 ? 31 : (int)(fuzz_rand() % 32);
    for (int i = 0; i < len; i++) car.name[i] = "abcdefghijklmnopqrstuvwxyz_0123456789"[fuzz_rand() % 37];
    car.name[len] = 0;
    car.mass = plausible(500.0f, 2500.0f);
    for (int i = 0; i < 4; i++) random_wheel(&car.wheels[i]);
    W.ts.vtable = g_ts_vtable;
    W.tire.mass = plausible(5.0f, 20.0f);
    W.tire.inertia = plausible(0.5f, 2.0f);
    W.tire.load_factor = plausible(0.8f, 1.5f);
    random_bytes(&W.p0, 5);
    random_bytes(&W.p1, 5);
    random_bytes(&W.out, 5);
    // the car file, mostly plausible
    CarData* cd = (CarData*)W.cd;
    for (int r = 0; r < 2; r++) {
        cd->track[r] = plausible(50.0f, 70.0f);
        cd->ride_height[r] = plausible(2.0f, 8.0f);
        cd->springs[r] = plausible(100.0f, 1000.0f);
        cd->bump[r] = plausible(0.0f, 20.0f);
        cd->rebound[r] = plausible(0.0f, 20.0f);
        cd->camber[r] = plausible(-3.0f, 3.0f);
        cd->toe[r] = plausible(-1.0f, 1.0f);
        cd->anti[r] = plausible(0.0f, 1.0f);
        cd->bump_camber[r] = plausible(-2.0f, 2.0f);
        cd->bump_toe[r] = plausible(-2.0f, 2.0f);
        cd->toe_compliance[r] = plausible(0.0f, 0.5f);
        cd->camber_compliance[r] = plausible(0.0f, 0.5f);
        cd->tyre[r].width = plausible(200.0f, 350.0f);
        cd->tyre[r].aspect = plausible(30.0f, 60.0f);
        cd->tyre[r].rim = plausible(14.0f, 19.0f);
        cd->brake[r] = plausible(500.0f, 3000.0f);
    }
    cd->wheelbase = plausible(90.0f, 120.0f);
    cd->weight_distribution = plausible(40.0f, 60.0f);
    cd->caster = plausible(0.0f, 8.0f);
    cd->cm_height = plausible(10.0f, 25.0f);
    uint32_t k = fuzz_rand() % 8;
    cd->torque_balance = k == 0 ? 0.0f : k == 1 ? 1.0f : k == 2 ? 0.9f : k == 3 ? 0.1f : k == 4 ? -0.5f : plausible(0.0f, 1.0f);
    cd->rolling_resistance = plausible(0.0f, 0.05f);
    cd->static_rolling_resistance = plausible(0.0f, 0.05f);
    W.hub.x = plausible(-1.0f, 1.0f);
    W.hub.y = plausible(-0.5f, 0.5f);
    W.hub.z = plausible(-2.0f, 2.0f);
    *g_temperature = plausible(250.0f, 320.0f);
    g_resource_exists = (uint8_t)(fuzz_rand() & 1);
}

// ---- one check: the original, then the rewrite, from the same world -------------------------------------------
static uint32_t g_ret_orig, g_ret_new;
static const char* g_name;
static int g_iter;

static void begin() {
    memcpy(&g_snap, &g_world, sizeof(World));
    g_temp_snap = *g_temperature;
    g_log.n = 0;
}
static void mid() {
    memcpy(&g_after, &g_world, sizeof(World));
    g_log_orig = g_log;
    memcpy(&g_world, &g_snap, sizeof(World));
    *g_temperature = g_temp_snap;
    g_log.n = 0;
}
static bool finish() {
    bool ok = true;
    const uint8_t* a = (const uint8_t*)&g_after;
    const uint8_t* b = (const uint8_t*)&g_world;
    for (size_t i = 0; i < sizeof(World); i++)
        if (a[i] != b[i]) {
            size_t o = i & ~3u;
            uint32_t x, y;
            memcpy(&x, a + o, 4);
            memcpy(&y, b + o, 4);
            const char* where = "world";
            size_t rel = o;
            if (o >= offsetof(Car, wheels) && o < offsetof(Car, wheels) + sizeof(Wheel) * 4) {
                where = "wheel";
                rel = (o - offsetof(Car, wheels)) % sizeof(Wheel);
            }
            printf("  %s iteration %d: %s +0x%zx original %08x (%.9g), rewrite %08x (%.9g)\n", g_name, g_iter, where,
                   rel, x, fbits(x), y, fbits(y));
            ok = false;
            break;
        }
    if (g_log.n != g_log_orig.n || memcmp(g_log.w, g_log_orig.w, g_log.n * 4)) {
        printf("  %s iteration %d: the calls differ (%u words vs %u)\n", g_name, g_iter, g_log_orig.n, g_log.n);
        for (uint32_t i = 0; i < g_log.n || i < g_log_orig.n; i++)
            printf("    %08x %08x\n", i < g_log_orig.n ? g_log_orig.w[i] : 0, i < g_log.n ? g_log.w[i] : 0);
        ok = false;
    }
    if (g_ret_orig != g_ret_new) {
        printf("  %s iteration %d: returns %08x vs %08x\n", g_name, g_iter, g_ret_orig, g_ret_new);
        ok = false;
    }
    return ok;
}

// the v1.0 originals, by address
typedef void(__fastcall* W0_t)(Wheel*, Edx);
typedef void(__fastcall* WSetup_t)(Wheel*, Edx, const CarData*, Car*, int, const P3*);
typedef void(__fastcall* WHeat_t)(Wheel*, Edx, float);
typedef void(__fastcall* WCar_t)(Wheel*, Edx, Car*);
typedef void(__fastcall* WMsg_t)(Wheel*, Edx, WheelMessage*, Car*);
typedef void(__fastcall* WReplay_t)(Wheel*, Edx, const WheelReplayPacket*, const WheelReplayPacket*, float, Car*);
typedef void(__fastcall* WPacket_t)(const Wheel*, Edx, WheelReplayPacket*);
typedef void(__fastcall* WTorque_t)(Wheel*, Edx, uint32_t);
typedef float(__fastcall* WRpm_t)(const Wheel*, Edx);

#ifdef VP_TEST_FIXES
// Wheel::Setup's FIX, for a 31-character car name: the original has just run (g_after, g_log_orig) and the rewrite
// (g_world, g_log). The original put its terminator on the tyre width's low byte; so the width it passed to
// TireCreate is replaced with the one it passes for the same world with the name a character shorter (which it
// computes the same way, but doesn't overrun), and then everything must match -- the file names included.
static long c_long_names, c_long_width_clobbered;
static World g_rw;
static int tire_width_at(const CallLog& l) {
    for (uint32_t i = 0; i + 1 < l.n; i++)
        if (l.w[i] == 'TIRE') return (int)i + 1;
    return -1;
}
static bool check_long_name_setup(Wheel* w, const CarData* cd, int index, const P3* hub) {
    c_long_names++;
    const int po = tire_width_at(g_log_orig), pn = tire_width_at(g_log);
    if (po < 0 || pn < 0 || po != pn) { printf("  Wheel::Setup iteration %d: no TireCreate to compare\n", g_iter); return false; }
    memcpy(&g_rw, &g_world, sizeof(World));                 // the rewrite's result and log, kept
    CallLog rw_log = g_log;
    memcpy(&g_world, &g_snap, sizeof(World));
    *g_temperature = g_temp_snap;
    g_world.car.name[30] = 0;
    g_log.n = 0;
    ((void(__fastcall*)(Wheel*, Edx, const CarData*, Car*, int, const P3*))0x00448140)(w, 0, cd, &g_world.car, index, hub);
    const int p30 = tire_width_at(g_log);
    if (p30 != po) { printf("  Wheel::Setup iteration %d: the shorter name's calls differ\n", g_iter); return false; }
    const uint32_t w30 = g_log.w[p30];
    if (g_log_orig.w[po] != w30) c_long_width_clobbered++;
    g_log_orig.w[po] = w30;
    memcpy(&g_world, &g_rw, sizeof(World));
    g_log = rw_log;
    return finish();
}
#endif

static int run_one(int which, int it) {
    random_world();
    Car& car = g_world.car;
    int wi = (int)(fuzz_rand() % 4);
    Wheel* w = &car.wheels[wi];
    g_iter = it;
    g_ret_orig = g_ret_new = 0;
    switch (which) {
    case 0: {                                                   // Wheel::Wheel on random bytes
        g_name = "Wheel::Wheel";
        random_bytes(w, sizeof(Wheel));
        begin();
        g_ret_orig = (uint32_t)((WheelCtor_t)0x00447ed0)(w, 0);
        mid();
        g_ret_new = (uint32_t)Wheel_ctor(w, 0);
        break;
    }
    case 1:
        g_name = "Wheel::ResetPosition";
        begin(); ((W0_t)0x004480c0)(w, 0); mid(); Wheel_ResetPosition(w, 0);
        break;
    case 2:
        g_name = "Wheel::~Wheel";
        begin(); ((W0_t)0x00448100)(w, 0); mid(); Wheel_dtor(w, 0);
        break;
    case 3: {
        g_name = "Wheel::Setup";
        orig_ctor(w, 0);                                        // Setup follows construction
        int index = (fuzz_rand() % 8) ? wi : (int)(fuzz_rand() % 8) - 2;
        const P3* hub = &g_world.hub;
        const CarData* cd = (const CarData*)g_world.cd;
        begin(); ((WSetup_t)0x00448140)(w, 0, cd, &car, index, hub); mid(); Wheel_Setup(w, 0, cd, &car, index, hub);
#ifdef VP_TEST_FIXES
        if (strlen(car.name) == 31) return check_long_name_setup(w, cd, index, hub) ? 0 : 1;
#endif
        break;
    }
    case 4: {
        g_name = "Wheel::UpdateHeat";
        float dt = (fuzz_rand() & 1) ? 0.128f : fuzz_float();
        begin(); ((WHeat_t)0x0044a440)(w, 0, dt); mid(); Wheel_UpdateHeat(w, 0, dt);
        break;
    }
    case 5:
        g_name = "Wheel::update_wheel_position";
        begin(); ((WCar_t)0x0044a490)(w, 0, &car); mid(); Wheel_update_wheel_position(w, 0, &car);
        break;
    case 6:
        g_name = "Wheel::UpdateCommon";
        begin(); ((WCar_t)0x0044a630)(w, 0, &car); mid(); Wheel_UpdateCommon(w, 0, &car);
        break;
    case 7:
        g_name = "Wheel::GetMessage";
        begin(); ((WMsg_t)0x0044a650)(w, 0, &g_world.msg, &car); mid(); Wheel_GetMessage(w, 0, &g_world.msg, &car);
        break;
    case 8: {
        g_name = "Wheel::UpdateReplay";
        uint32_t k = fuzz_rand() % 4;
        float t = k == 0 ? 0.0f : k == 1 ? 1.0f : k == 2 ? plausible(0.0f, 1.0f) : fuzz_float();
        begin(); ((WReplay_t)0x0044a770)(w, 0, &g_world.p0, &g_world.p1, t, &car);
        mid(); Wheel_UpdateReplay(w, 0, &g_world.p0, &g_world.p1, t, &car);
        break;
    }
    case 9:
        g_name = "Wheel::MakeReplayPacket";
        begin(); ((WPacket_t)0x0044a960)(w, 0, &g_world.out); mid(); Wheel_MakeReplayPacket(w, 0, &g_world.out);
        break;
    case 10: {
        g_name = "Wheel::ApplyTorque";
        float tq = fuzz_float();
        uint32_t u = bits(tq);
        begin(); ((WTorque_t)0x0044ab20)(w, 0, u); mid(); Wheel_ApplyTorque(w, 0, u);
        break;
    }
    case 11: {
        g_name = "Wheel::GetRPM";
        begin();
        float a = ((WRpm_t)0x0044ab30)(w, 0);
        memcpy(&g_ret_orig, &a, 4);
        mid();
        float b = Wheel_GetRPM(w, 0);
        memcpy(&g_ret_new, &b, 4);
        break;
    }
    }
    return finish() ? 0 : 1;
}

static const char* const k_names[] = {"Wheel::Wheel", "Wheel::ResetPosition", "Wheel::~Wheel", "Wheel::Setup",
                                      "Wheel::UpdateHeat", "Wheel::update_wheel_position", "Wheel::UpdateCommon",
                                      "Wheel::GetMessage", "Wheel::UpdateReplay", "Wheel::MakeReplayPacket",
                                      "Wheel::ApplyTorque", "Wheel::GetRPM"};

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_FUZZ_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 100000;
    const char* filter = argc > 2 && strcmp(argv[2], "-") ? argv[2] : 0;
    if (argc > 3) g_state = (uint32_t)strtoul(argv[3], 0, 0) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);                        // ...\test\world_wheel2.cpp (built /FC) -> ...\out\race_v10.exe
    char* s = strstr(exe, "\\test\\world_wheel2.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    patch_jump(0x0043c080, (void*)&stub_TireCreate);
    patch_jump(0x004731c0, (void*)&stub_TireSoundCreate);
    patch_jump(0x00419d10, (void*)&stub_ResourceExists);
    patch_jump(0x00414390, (void*)&stub_delete);
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);           // the physics thread's mode (ExceptSinglePrecision)
    int failed = 0;
    for (int which = 0; which < 12; which++) {
        if (filter && !strstr(k_names[which], filter)) continue;
        int bad = 0;
        for (int it = 0; it < iterations && bad < 3; it++) bad += run_one(which, it);
        printf("  %-30s %s\n", k_names[which], bad ? "DIFFERS (above)" : "identical to the original");
        if (bad) failed++;
    }
#ifdef VP_TEST_FIXES
    printf("fix build: Wheel::Setup with a 31-character car name %ld times (the original's width clobbered %ld)\n",
           c_long_names, c_long_width_clobbered);
    if (!filter && !c_long_names) failed++;
#endif
    printf("%d of 12 differ (%d random worlds each)\n", failed, iterations);
    return failed ? 1 : 0;
}
