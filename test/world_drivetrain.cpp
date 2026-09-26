// world_drivetrain.cpp -- the drivetrain rewrites (hook/phys_drivetrain.cpp) against the originals, on real
// drivetrains built in a fake Car, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_drivetrain.cpp
//        /Fo%TEMP%\wd\ /Fe%TEMP%\wd\world_drivetrain.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_drivetrain.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. Each world is a Car-sized buffer with the
// v1.0 part layout -- wheels +1364, Engine +3068, Clutch +3196, Transmission +3224, centre/front/rear
// Differential +3332/+3364/+3396 -- built with the ORIGINAL constructors and Setup/Connect calls, as
// Car::Setup does, then randomised (rpms, torques, gears, the auto-clutch state, throttle and clutch, with
// occasional NaN / inf / denormal / signalling-NaN values). Every part's vtable is swapped for a logging copy
// (the same original functions behind a thunk that records each GetRPM / ApplyTorque call, its object and its
// value), so the virtual calls' order and arguments are compared too. Then a run of Car::Update-like steps
// and single calls: for each, the original runs, the world is restored, the rewrite runs, and the whole car,
// the return value and the call logs are compared bit for bit; the bytes the original changed must lie
// inside the rewrite's footprint. The world then continues from the original's result.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
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

// ---- what the rewrites link against, standing in for the DLL ------------------------------------------
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
static int g_fp_unknown;
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) {
    uint32_t vt = *(uint32_t*)obj;
    if (vt == 0x004dbfd0) add(obj, 3768, what);            // Car (state_layout.inc)
    else { printf("footprint: %s at %p has an unknown class (vtable %08x)\n", what, obj, vt); g_fp_unknown++; }
}

#include "../hook/phys_drivetrain.cpp"

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
static int g_special_pct = 3;                                 // % of values replaced by a special
static float special() {
    switch (rnd() % 8) {
    case 0: return fbits(0x7fc00000u | (rnd() & 0x3fffff));                    // quiet NaN
    case 1: return fbits(0xffc00000u | (rnd() & 0x3fffff));                    // negative quiet NaN
    case 2: return fbits(0x7f800001u + (rnd() & 0x3ffffe));                    // signalling NaN
    case 3: return fbits(0x7f800000u | (rnd() & 0x80000000u));                 // +-inf
    case 4: return fbits(rnd() & 0x807fffffu);                                 // denormal
    case 5: return (rnd() & 1) ? 0.0f : -0.0f;
    case 6: return fbits(rnd() & 0x80000000u | 0x7f7fffffu);                    // +-FLT_MAX
    default: return fbits(rnd());                                               // any bits
    }
}
static float sp(float x) { return (int)(rnd() % 100) < g_special_pct ? special() : x; }

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

// ---- the world: a Car-sized buffer ----------------------------------------------------------------------------
enum {
    CAR_BUF = 4096,                                   // the Car base is 3768; the tail must never change
    OFF_WHEELS = 1364, WHEEL_SIZE = 0x1a4,
    OFF_ENGINE = 3068, OFF_CLUTCH = 3196, OFF_TRANS = 3224, OFF_CDIFF = 3332, OFF_FDIFF = 3364, OFF_RDIFF = 3396,
};
static_assert(OFF_ENGINE + sizeof(Engine) == OFF_CLUTCH && OFF_CLUTCH + sizeof(Clutch) == OFF_TRANS &&
              OFF_TRANS + sizeof(Transmission) == OFF_CDIFF && OFF_CDIFF + 32 == OFF_FDIFF && OFF_FDIFF + 32 == OFF_RDIFF,
              "Car part layout");
static uint8_t* g_car;
static PhobDyno* car() { return (PhobDyno*)g_car; }
static Engine* engine() { return (Engine*)(g_car + OFF_ENGINE); }
static Clutch* clutch() { return (Clutch*)(g_car + OFF_CLUTCH); }
static Transmission* trans() { return (Transmission*)(g_car + OFF_TRANS); }
static Differential* diff(int i) { return (Differential*)(g_car + OFF_CDIFF + 32 * i); }   // 0 centre, 1 front, 2 rear
static uint8_t* wheel(int i) { return g_car + OFF_WHEELS + WHEEL_SIZE * i; }

static const char* part_of(uint32_t off, uint32_t* rel) {
    struct { uint32_t o, n; const char* name; } parts[] = {
        {0, 0x478, "PhobDyno"}, {OFF_WHEELS, 4 * WHEEL_SIZE, "wheels"}, {OFF_ENGINE, 128, "engine"},
        {OFF_CLUTCH, 28, "clutch"}, {OFF_TRANS, 108, "transmission"}, {OFF_CDIFF, 32, "centre_diff"},
        {OFF_FDIFF, 32, "front_diff"}, {OFF_RDIFF, 32, "rear_diff"}};
    for (auto& p : parts)
        if (off >= p.o && off < p.o + p.n) { *rel = off - p.o; return p.name; }
    *rel = off;
    return "car";
}

// ---- logging vtables: the original functions behind a thunk that records each call ---------------------
struct LogEntry { char kind; uint32_t self; uint64_t v; };
enum { LOG_MAX = 4096 };
static LogEntry g_log[2][LOG_MAX];
static int g_nlog[2], g_pass;
static void log_add(char k, void* s, const void* v, int n) {
    if (g_nlog[g_pass] >= LOG_MAX) return;
    LogEntry& e = g_log[g_pass][g_nlog[g_pass]++];
    e.kind = k;
    e.self = (uint32_t)(uintptr_t)s;
    e.v = 0;
    memcpy(&e.v, v, n);
}
template <uint32_t REAL> static double __fastcall lg_rpm(void* self, int) {
    double r = ((double(__fastcall*)(void*, int))REAL)(self, 0);
    log_add('G', self, &r, 8);
    return r;
}
template <uint32_t REAL> static void __fastcall lg_apply(void* self, int, uint32_t t) {
    log_add('A', self, &t, 4);
    ((void(__fastcall*)(void*, int, uint32_t))REAL)(self, 0, t);
}
static void* vt_wheel[2] = {(void*)&lg_rpm<0x44ab30>, (void*)&lg_apply<0x44ab20>};
static void* vt_engine[2] = {(void*)&lg_rpm<0x445ea0>, (void*)&lg_apply<0x446810>};
static void* vt_trans[2] = {(void*)&lg_rpm<0x4464c0>, (void*)&lg_apply<0x4464f0>};
static void* vt_diff[2] = {(void*)&lg_rpm<0x445c60>, (void*)&lg_apply<0x445c90>};

// ---- the originals used to build a world ----------------------------------------------------------------
typedef void*(__fastcall* Ctor_t)(void*, int);
typedef void(__fastcall* PowerCurveSetup_t)(void*, int, float, float, float, float, float);
typedef void(__fastcall* EngineSetup_t)(void*, int, PhobDyno*, float, float, const void*, float, float);
typedef void(__fastcall* DiffSetup_t)(void*, int, PhobDyno*, float, float);
typedef void(__fastcall* Connect2_t)(void*, int, void*, void*);
typedef void(__fastcall* ClutchSetup_t)(void*, int, PhobDyno*, float);
typedef void(__fastcall* TransSetup_t)(void*, int, PhobDyno*, float, float, int, const float*, void*, void*);
typedef void(__fastcall* Connect1_t)(void*, int, void*);

static void build_world() {
    // everything random first (floats from a mix), then the parts over it as Car::Setup builds them
    for (int i = 0; i < CAR_BUF; i += 4) {
        float x = (rnd() & 1) ? rf(-1, 1) : rf(-3000, 3000);
        memcpy(g_car + i, &x, 4);
    }
    *(uint32_t*)g_car = 0x004dbfd0;                                   // Car's vtable (footprints size it)
    for (int i = 0; i < 4; i++) {
        *(void***)wheel(i) = vt_wheel;
        *(float*)(wheel(i) + 0x128) = sp(rf(-3000, 3000));           // Wheel::GetRPM
    }
    ((Ctor_t)0x00446630)(engine(), 0);
    uint8_t curve[24];
    ((PowerCurveSetup_t)0x00446590)(curve, 0, rf(150, 600), rf(4000, 7000), rf(150, 600), rf(2000, 4500),
                                    rf(0, 60) * 1.3515152f);
    float redline = rf(5000, 8500), idle = rf(500, 1200);
    ((EngineSetup_t)0x00446700)(engine(), 0, car(), rf(0.05f, 0.6f), rf(0.001f, 0.03f), curve, redline, idle);
    ((Ctor_t)0x00445d00)(clutch(), 0);
    ((Ctor_t)0x00445f60)(trans(), 0);
    for (int i = 0; i < 3; i++) ((Ctor_t)0x00445c00)(diff(i), 0);
    float ratios[8];
    int n = ri(1, 7);
    float r = rf(2.5f, 4.0f);
    for (int i = 0; i < 8; i++) { ratios[i] = r; r *= rf(0.6f, 0.85f); }
    ((TransSetup_t)0x00445fb0)(trans(), 0, car(), 0x1.b0f22p-3f, rf(0.0f, 0.02f), n, ratios, engine(), clutch());
    ((ClutchSetup_t)0x00445d20)(clutch(), 0, car(), rf(150, 600) * 4.0f);
    float fd = rf(2.5f, 4.5f);
    ((DiffSetup_t)0x00445c20)(diff(1), 0, car(), fd, rf(0, 60) * 1.35725f);
    ((DiffSetup_t)0x00445c20)(diff(2), 0, car(), fd, rf(0, 60) * 1.35725f);
    ((DiffSetup_t)0x00445c20)(diff(0), 0, car(), 1.0f, rf(0, 60) * 1.35725f);
    ((Connect2_t)0x00445c40)(diff(1), 0, wheel(0), wheel(1));
    ((Connect2_t)0x00445c40)(diff(2), 0, wheel(2), wheel(3));
    ((Connect2_t)0x00445c40)(diff(0), 0, diff(1), diff(2));
    int out = ri(0, 2);                                               // front, rear or centre (torque balance)
    ((Connect1_t)0x00445fa0)(trans(), 0, out == 0 ? diff(1) : out == 1 ? diff(2) : diff(0));
    ((Connect2_t)0x00445d40)(clutch(), 0, engine(), trans());
    // logging vtables
    engine()->vtable = vt_engine;
    trans()->vtable = vt_trans;
    for (int i = 0; i < 3; i++) diff(i)->vtable = vt_diff;
    // state
    Engine* e = engine();
    e->rpm = sp(rf(-500, 9000));
    e->throttle = sp(uni());
    e->smoothed_throttle = sp(rf(0, 1.2f));
    e->stalled = rnd() % 5 == 0;
    e->cranking = rnd() % 3 == 0;
    Clutch* c = clutch();
    c->engagement = sp(uni());
    c->torque = sp(rf(-2000, 2000));
    Transmission* t = trans();
    t->rpm = sp(rf(-1000, 9000));
    t->gear = ri(-1, n + 1);
    t->engaged_gear = ri(-1, n);
    t->ratio = sp(rnd() % 4 ? ratios[ri(0, n - 1)] : rf(-4, 4));
    t->input_rpm = sp(rf(-1000, 9000));
    t->in_gear = t->engaged_gear != 0 ? 1 : (uint8_t)(rnd() % 4 == 0);
    t->clutch = sp(rf(-0.2f, 1.2f));
    t->clutch_applied = sp(rf(-0.3f, 1.3f));
    t->throttle = sp(uni());
    t->shift_state = rnd() % 16 == 0 ? ri(-1, 7) : ri(0, 5);
    for (int i = 0; i < 3; i++) { diff(i)->rpm_a = sp(rf(-3000, 3000)); diff(i)->rpm_b = sp(rf(-3000, 3000)); }
    // the car's rotation row 2 (the shafts' reaction axis) and torque accumulator
    for (int i = 0; i < 3; i++) car()->frame.rot.m[6 + i] = sp(rf(-1, 1));
    for (int i = 0; i < 3; i++) (&car()->torque.x)[i] = sp(rf(-5000, 5000));
}

// ---- running one call both ways ----------------------------------------------------------------------------
template <typename F, typename... A> static uint64_t invoke(F f, A... a) {
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

struct Stat { const char* name; long calls, changed, fails, fp_fails; };
static Stat g_stats[64];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    g_stats[g_nstats] = {name, 0, 0, 0, 0};
    return g_stats[g_nstats++];
}
static uint8_t g_start[CAR_BUF], g_after[CAR_BUF];
static Footprint g_fp;
static int g_world;

template <typename Run, typename Fp> static void check(const char* name, Run run, Fp footprint) {
    Stat& st = stat(name);
    st.calls++;
    memcpy(g_start, g_car, CAR_BUF);
    g_fp.n = 0;
    footprint(g_fp);
    g_pass = 0; g_nlog[0] = 0;
    uint64_t ro = run(true);
    memcpy(g_after, g_car, CAR_BUF);
    if (memcmp(g_after, g_start, CAR_BUF)) st.changed++;
    memcpy(g_car, g_start, CAR_BUF);
    g_pass = 1; g_nlog[1] = 0;
    uint64_t rn = run(false);
    bool bad = ro != rn || memcmp(g_after, g_car, CAR_BUF) || g_nlog[0] != g_nlog[1] ||
               memcmp(g_log[0], g_log[1], sizeof(LogEntry) * g_nlog[0]);
    if (bad && st.fails++ < 4) {
        printf("MISMATCH %s (world %d)\n", name, g_world);
        if (ro != rn) printf("  return: original %016llx, rewrite %016llx\n", ro, rn);
        for (uint32_t i = 0; i < CAR_BUF; i += 4)
            if (memcmp(g_after + i, g_car + i, 4)) {
                uint32_t rel, a, b, s;
                memcpy(&a, g_after + i, 4); memcpy(&b, g_car + i, 4); memcpy(&s, g_start + i, 4);
                const char* p = part_of(i, &rel);
                printf("  %s+0x%x: start %08x, original %08x (%.9g), rewrite %08x (%.9g)\n", p, rel, s, a, fbits(a), b,
                       fbits(b));
            }
        int nl = g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1];
        for (int i = 0; i < nl; i++) {
            LogEntry* a = i < g_nlog[0] ? &g_log[0][i] : 0;
            LogEntry* b = i < g_nlog[1] ? &g_log[1][i] : 0;
            if (a && b && !memcmp(a, b, sizeof *a)) continue;
            uint32_t rel;
            if (a) printf("  call %d original: %c %s %016llx\n", i, a->kind, part_of(a->self - (uint32_t)(uintptr_t)g_car, &rel), a->v);
            if (b) printf("  call %d rewrite:  %c %s %016llx\n", i, b->kind, part_of(b->self - (uint32_t)(uintptr_t)g_car, &rel), b->v);
        }
    }
    // the footprint must cover every byte the original changed
    for (uint32_t i = 0; i < CAR_BUF; i++) {
        if (g_after[i] == g_start[i]) continue;
        bool in = false;
        for (int k = 0; k < g_fp.n && !in; k++)
            in = g_car + i >= (uint8_t*)g_fp.r[k].p && g_car + i < (uint8_t*)g_fp.r[k].p + g_fp.r[k].n;
        if (!in) {
            if (st.fp_fails++ < 4) {
                uint32_t rel;
                const char* p = part_of(i, &rel);
                printf("FOOTPRINT %s (world %d): %s+0x%x changed outside it\n", name, g_world, p, rel);
            }
            break;
        }
    }
    memcpy(g_car, g_after, CAR_BUF);                                  // go on from the original's result
}

// CHECK(rewrite, args...): the arguments are evaluated once; the original is the same signature at the
// rewrite's registered address
#define CHECK(FN, ...)                                                                                  \
    do {                                                                                                \
        auto args_ = std::make_tuple(__VA_ARGS__);                                                      \
        check(VP_CAT(name_, FN),                                                                        \
              [&](bool orig) {                                                                          \
                  auto f = orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN;                    \
                  return std::apply([&](auto... a) { return invoke(f, a...); }, args_);                 \
              },                                                                                        \
              [&](Footprint& fp) { std::apply([&](auto... a) { VP_CAT(fpof_, FN)(fp, a...); }, args_); });\
    } while (0)

static int g_state_hits[8];                                           // Transmission::Update's starting shift_state

static void perturb() {
    for (int i = 0; i < 4; i++)
        if (rnd() % 3 == 0) *(float*)(wheel(i) + 0x128) = sp(rf(-3000, 3000));
    if (rnd() % 8 == 0) trans()->shift_state = ri(0, 5);
    if (rnd() % 16 == 0) engine()->stalled ^= 1;
    if (rnd() % 10 == 0) engine()->rpm = sp(rf(-500, 9000));
}

static void run_world() {
    build_world();
    Transmission* t = trans();
    Clutch* c = clutch();
    int steps = ri(4, 30);
    for (int s = 0; s < steps; s++) {
        perturb();
        // Car::Update's order: throttle, clutch, Transmission::Update, (clutch forced), Clutch::Update
        if (rnd() % 6 == 0) CHECK(Transmission_SetGear, t, 0, ri(-1, t->num_gears + 1));
        if (rnd() % 4 == 0) CHECK(Transmission_SetGearAuto, t, 0);
        CHECK(Transmission_SetThrottle, t, 0, sp(uni()));
        CHECK(Transmission_SetClutch, t, 0, sp(rf(-0.2f, 1.2f)));
        g_state_hits[(uint32_t)t->shift_state < 6 ? t->shift_state : 6]++;
        CHECK(Transmission_Update, t, 0, (uint8_t)(rnd() % 4 != 0 ? 1 : 0));
        if (rnd() % 8 == 0) CHECK(Clutch_SetClutch, c, 0, sp(uni()));
        CHECK(Clutch_Update, c, 0);
        // the other entry points, at random
        switch (rnd() % 12) {
        case 0: CHECK(Transmission_GetRPM, t, 0); break;
        case 1: CHECK(Transmission_ApplyTorque, t, 0, sp(rf(-2000, 2000))); break;
        case 2: CHECK(Differential_GetRPM, diff(ri(0, 2)), 0); break;
        case 3: CHECK(Differential_ApplyTorque, diff(ri(0, 2)), 0, sp(rf(-5000, 5000))); break;
        case 4: CHECK(Shaft_ApplyTorque, (Shaft*)engine(), 0, sp(rf(-800, 800))); break;
        case 5: CHECK(Shaft_ApplyTorque, (Shaft*)t, 0, sp(rf(-800, 800))); break;
        case 6: CHECK(Shaft_GetRPM, (Shaft*)t, 0); break;
        case 7: CHECK(Shaft_GetInertialTorque, (Shaft*)t, 0, sp(rf(-9000, 9000))); break;
        case 8: CHECK(Shaft_GetDragTorque, (Shaft*)engine(), 0, sp(rf(-9000, 9000))); break;
        case 9: CHECK(Transmission_ApplyTorque, t, 0, sp(rf(-2000, 2000))); break;
        default: break;
        }
    }
    // the set-up calls, which overwrite the parts: last
    float ratios[16];
    for (int i = 0; i < 16; i++) ratios[i] = sp(rf(0.3f, 4.0f));
    switch (rnd() % 5) {
    case 0: CHECK(Transmission_Setup, t, 0, car(), sp(rf(0.01f, 1)), sp(rf(0, 0.02f)), ri(-2, 8), ratios, engine(), c); break;
    case 1: CHECK(Clutch_Setup, c, 0, car(), sp(rf(0, 3000))); break;
    case 2: CHECK(Differential_Setup, diff(ri(0, 2)), 0, car(), sp(rf(0, 5)), sp(rf(0, 80))); break;
    case 3: CHECK(Shaft_Setup, (Shaft*)t, 0, car(), sp(rf(0.01f, 1)), sp(rf(0, 0.02f))); break;
    default: CHECK(Shaft_Setup, (Shaft*)engine(), 0, car(), sp(rf(-1, 1)), sp(rf(0, 0.02f))); break;
    }
    switch (rnd() % 3) {
    case 0: CHECK(Transmission_Connect, t, 0, (TorqueInput*)diff(ri(0, 2))); break;
    case 1: CHECK(Clutch_Connect, c, 0, (TorqueInput*)engine(), (TorqueInput*)t); break;
    default: CHECK(Differential_Connect, diff(ri(0, 2)), 0, (TorqueInput*)wheel(ri(0, 3)), (TorqueInput*)wheel(ri(0, 3))); break;
    }
    switch (rnd() % 5) {
    case 0: CHECK(Transmission_Ctor, t, 0); break;
    case 1: CHECK(Clutch_Ctor, c, 0); break;
    case 2: CHECK(Differential_Ctor, diff(ri(0, 2)), 0); break;
    case 3: CHECK(Shaft_Ctor, (Shaft*)t, 0); break;
    default: CHECK(Shaft_Ctor, (Shaft*)engine(), 0); break;
    }
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 100000;
    if (argc > 2) g_state = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);                                    // ...\test\world_drivetrain.cpp (built /FC)
    char* s = strstr(exe, "\\test\\world_drivetrain.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    g_car = (uint8_t*)VirtualAlloc(0, CAR_BUF, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    // race.exe's data (.data and .bss): nothing here may write it
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(0x400000 + ((IMAGE_DOS_HEADER*)0x400000)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* data = 0;
    uint32_t data_n = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".data", 6)) { data = (uint8_t*)0x400000 + sec[i].VirtualAddress; data_n = sec[i].Misc.VirtualSize; }
    uint8_t* data_copy = (uint8_t*)malloc(data_n);
    memcpy(data_copy, data, data_n);
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);                       // the physics thread's mode
    for (g_world = 0; g_world < worlds; g_world++) {
        g_special_pct = g_world % 4 == 0 ? 0 : g_world % 4 == 1 ? 1 : 5;
        run_world();
    }
    int failed = 0;
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("  %-28s %8ld calls, %8ld changed the car: %s%s\n", st.name, st.calls, st.changed,
               st.fails ? "DIFFERS" : "identical",
               st.fp_fails ? "  FOOTPRINT MISSES" : "");
        failed += st.fails || st.fp_fails;
    }
    printf("  Transmission::Update starting shift_state 0..5, other: %d %d %d %d %d %d %d\n", g_state_hits[0],
           g_state_hits[1], g_state_hits[2], g_state_hits[3], g_state_hits[4], g_state_hits[5], g_state_hits[6]);
    bool data_ok = !memcmp(data_copy, data, data_n);
    printf("%d worlds; %d of %d functions differ; race.exe .data %s; %d unknown footprint classes\n", worlds, failed,
           g_nstats, data_ok ? "untouched" : "WRITTEN", g_fp_unknown);
    return failed || !data_ok || g_fp_unknown ? 1 : 0;
}
