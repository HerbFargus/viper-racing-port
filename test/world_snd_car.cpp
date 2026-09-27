// world_snd_car.cpp -- sound group B's rewrites (hook/snd_car.cpp: EngineSound, EngineSoundSample, TireSound,
// RealTireSound, DummyTireSound and the two object files' $E initialisers) against the originals, on random worlds,
// outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_snd_car.cpp
//        /Fo<dir>\ /Fe<dir>\world_snd_car.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_snd_car.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. Each world
// builds, in an arena: a Car (the real vtable 0x4dbfd0, so GetPerceivedThrottle / GetPerceivedRPM and
// PhysReplayPlayMode run as the game's code; its perceived RPM and throttle, index and name random), an EngineSound
// with its 7 bands, an EngineSoundSample, up to 16 Sound3Ds (a fake vtable whose deleting destructor logs), four
// wheels (slide, surface class, contact point) and six tyre sounds (Real / Dummy, their wheel_sound links random:
// NULL, themselves, each other), a fake .ens resource and engine.txt lines; and the game's globals these read (the
// focus and player cars, the mixer quality, the replay flag, the tyre-sound table at 0x578930). Floats are drawn
// around the thresholds the code tests (1600 RPM, 0.1 throttle, the bands' RPM corners, 0.25 idle volume, a zero
// slide) with NaNs, infinities, denormals and huge values mixed in; one world in ten replaces them wholesale.
//
// Stubbed (a jump to a logger): MemAlloc (a bump heap in the arena, sometimes NULL), operator delete, LogReport,
// LogPanic (returns), sprintf and sscanf (the host's), ResourceTry / ResourceGet / ResourceExists / ResourceForget
// (scripted), FileOpen / FileReadLine / FileClose (scripted lines), Sound3D::Create (a fake sound from the arena, or
// NULL). Everything else runs as the game's code, including this file's functions where one calls another by address
// (EngineSound::Update -> EngineSoundSample::Update -> GetVolume, RealTireSound's destructor, the constructors).
//
// The arena and the image's whole .data/.bss are saved, the original runs, the result is kept, the world restored,
// the rewrite runs; the arena, .data/.bss, the return value and the stubs' logs (stack pointers logged by what they
// point at) are compared, and every byte the original changed must lie in the rewrite's footprint unless it's
// replay_only (no static is exempt: the sound library's aren't in the physics thread's auto-save). The FPU runs at
// 24 or 53 bits, in 30% of the worlds with overflow and divide-by-zero unmasked and in another 10% with the
// denormal-operand exception unmasked as well (ExceptDiv0Crashes(1), which the physics / BG thread runs with): a
// fault must happen in both passes. Each pass starts and ends with fninit.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#include "../hook/port.h"
// every PORT_FN: its address, name, rewrite and footprint, in a list
struct SndReg {
    uint32_t v10; const char* name; void* fn; void* fp; SndReg* next;
    static SndReg*& head() { static SndReg* h; return h; }
    SndReg(uint32_t v, const char* n, void* f, void* p) : v10(v), name(n), fn(f), fp(p), next(head()) { head() = this; }
};
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;                                                      \
    static SndReg VP_CAT(reg_, NEW)(V10, NAME, (void*)&NEW, (void*)&FP);

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { printf("footprint: object() not expected (%s)\n", what); add(obj, 4, what); }
void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

#include "../hook/snd_car.cpp"

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
    case 1: return bitsf(0x7f800001u | (rnd() & 0x3fffff));        // a signalling NaN
    case 2: return s * INFINITY;
    case 3: return s * range(1e30f, 3e38f);
    case 4: return s * range(1e-38f, 1e-30f);
    case 5: return bitsf(rnd() & 0x807fffff);                      // a denormal
    case 6: return (r & 0x200) ? 0.0f : -0.0f;
    case 7: return s * range(1e6f, 1e9f);
    case 8: return s;
    case 9: return bitsf(0xffc00000u | (rnd() & 0x3fffff));        // a negative NaN
    default: return bitsf(rnd());
    }
}
// a float: mostly in [lo, hi), sometimes exactly one of the "near" values, sometimes wild
static float rf(float lo, float hi, int wild_pct = 3) { return chance(wild_pct) ? wildf() : range(lo, hi); }
static void wild_at(void* p) { float f = wildf(); memcpy(p, &f, 4); }

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

// ---- the world: an arena, and the image's .data/.bss ------------------------------------------------------------
enum : uint32_t {
    ARENA_BYTES = 0x10000,
    A_HEAP = 0x0000, A_HEAP_END = 0x4000,   // MemAlloc's bump heap
    A_CAR = 0x4000,                         // a Car (0xeb8)
    A_ES = 0x5000,                          // an EngineSound (0xf8; room past it for the overflow)
    A_SAMPLE = 0x5400,                      // an EngineSoundSample
    A_SND = 0x5800,                         // 16 Sound3Ds (0x40): the world's
    A_SVT = 0x5c00,                         // their vtable
    A_TS = 0x6000,                          // 6 tyre sounds, 0x40 apart
    A_WHEELS = 0x6400,                      // 4 Wheels, 0x1a4 each
    A_RES = 0x7000,                         // the .ens resource
    A_NEWSND = 0x8000, N_NEWSND = 16,       // Sound3D::Create's sounds (0x40 each)
};
enum { CAR_BYTES = 0xeb8, WHEEL_BYTES = 0x1a4 };
static uint8_t* g_arena;
static uint8_t* g_arena_snap;
static uint8_t* g_arena_after;
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* g_data_snap;
static uint8_t* g_data_after;
static uint8_t* AR(uint32_t off) { return g_arena + off; }
static uint8_t* snd(int i) { return AR(A_SND + 0x40 * i); }
static uint8_t* ts(int i) { return AR(A_TS + 0x40 * i); }
static uint8_t* wheel(int i) { return AR(A_WHEELS + WHEEL_BYTES * i); }

enum : uint32_t {
    X_FOCUS = 0x004f4358, X_PLAYER = 0x004f4354, X_QUALITY = 0x004f5934, X_REPLAY_PLAY = 0x005218e8,
    X_TIRE_TABLE = 0x00578930,
};

// ---- the stubs' log and script ------------------------------------------------------------------------------------
enum { LOG_MAX = 8192 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void log_word(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static void log_bytes(const void* p, int n) { for (int i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, (const uint8_t*)p + i, n - i < 4 ? n - i : 4); log_word(w); } }
static void log_str(const char* s) { size_t n = strnlen(s, 1024); log_word((uint32_t)n); log_bytes(s, (int)n); }

struct Script {
    uint32_t alloc_fail;            // bit per MemAlloc call: NULL
    uint32_t create_fail;           // bit per Sound3D::Create call: NULL
    uint32_t exists;                // bit per ResourceExists call
    uint8_t res_try, res_get;       // ResourceTry / ResourceGet find the resource
    int32_t file;                   // FileOpen's handle (0: missing)
    int nlines;                     // FileReadLine's lines; past them it fails
    char lines[10][300];
    uint8_t line_ok[10];            // FileReadLine's result for that line
};
static Script g_script;
static uint8_t* g_heap;
static int g_alloc_calls, g_create_calls, g_exists_calls, g_line_calls, g_newsnd;

static void* __cdecl stub_MemAlloc(int n) {
    log_word('MALC'); log_word((uint32_t)n);
    const int k = g_alloc_calls++;
    if (k < 32 && ((g_script.alloc_fail >> k) & 1)) { log_word(0); return 0; }
    if (n < 0 || (uint32_t)n > (uint32_t)(AR(A_HEAP_END) - g_heap)) { log_word(1); return 0; }
    uint8_t* p = g_heap;
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(0xa5 ^ i);           // garbage, as a fresh block has
    g_heap += ((uint32_t)n + 7) & ~7u;
    log_word((uint32_t)(p - g_arena));
    return p;
}
static void __cdecl stub_delete(void* p) { log_word('DELE'); log_word((uint32_t)p); }
static void __cdecl stub_log(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    buf[sizeof buf - 1] = 0;
    va_end(ap);
    log_word('LOGR'); log_word((uint32_t)fmt); log_str(buf);
}
static void __cdecl stub_panic(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    buf[sizeof buf - 1] = 0;
    va_end(ap);
    log_word('PANC'); log_word((uint32_t)fmt); log_str(buf);
}
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    log_word('SPRF'); log_word((uint32_t)fmt); log_str(buf);
    return r;
}
static int __cdecl stub_sscanf(const char* s, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    log_word('SSCN'); log_word((uint32_t)fmt); log_str(s);
    va_list aq = ap;
    for (int i = 0; i < 7; i++) log_word((uint32_t)va_arg(aq, void*));   // the 7 destinations (in the arena)
    unsigned cw;
    _controlfp_s(&cw, 0, 0);
    _controlfp_s(0, _MCW_EM, _MCW_EM);                                      // the host's parser: exceptions masked
    int r = vsscanf(s, fmt, ap);
    _controlfp_s(0, cw & _MCW_EM, _MCW_EM);
    va_end(ap);
    log_word((uint32_t)r);
    return r;
}
static uint8_t* __cdecl stub_resource(const char* name, uint32_t fourcc, uint32_t* size, int32_t* flags, uint8_t* a, uint8_t* b, bool get) {
    log_word(get ? 'RGET' : 'RTRY'); log_str(name); log_word(fourcc); log_word((uint32_t)a); log_word((uint32_t)b);
    const bool found = get ? g_script.res_get : g_script.res_try;
    if (found) { *size = 4 + 0x1c * (uint32_t)*(int32_t*)AR(A_RES); *flags = 0; }
    return found ? AR(A_RES) : 0;
}
static uint8_t* __cdecl stub_resource_try(const char* n, uint32_t f, uint32_t* s, int32_t* fl, uint8_t* a, uint8_t* b) { return stub_resource(n, f, s, fl, a, b, false); }
static uint8_t* __cdecl stub_resource_get(const char* n, uint32_t f, uint32_t* s, int32_t* fl, uint8_t* a, uint8_t* b) { return stub_resource(n, f, s, fl, a, b, true); }
static uint8_t __cdecl stub_resource_exists(const char* name) {
    const int k = g_exists_calls++;
    const uint8_t r = (uint8_t)((g_script.exists >> (k & 31)) & 1);
    log_word('REXI'); log_str(name); log_word(r);
    return r;
}
static uint8_t __cdecl stub_resource_forget(void* p) { log_word('RFGT'); log_word((uint32_t)p); return 1; }
static int32_t __cdecl stub_file_open(const char* name) { log_word('FOPN'); log_str(name); return g_script.file; }
static uint8_t __cdecl stub_file_read_line(int32_t fh, char* buf, int n) {
    const int k = g_line_calls++;
    log_word('FRDL'); log_word((uint32_t)fh); log_word((uint32_t)n);
    log_bytes(buf, n);                                                      // what the caller hands over (its zeroing)
    if (k >= g_script.nlines) return 0;
    const char* s = g_script.lines[k];
    int len = (int)strlen(s);
    if (len > n - 1) len = n - 1;
    memcpy(buf, s, len);
    buf[len] = 0;
    return g_script.line_ok[k];
}
static void __cdecl stub_file_close(int32_t* p) { log_word('FCLS'); log_word((uint32_t)*p); *p = 0; }
static uint8_t* __cdecl stub_sound3d_create(const char* name, int kind, const void* pos, const void* vel) {
    const int k = g_create_calls++;
    log_word('S3DC'); log_str(name); log_word((uint32_t)kind); log_word((uint32_t)pos); log_word((uint32_t)vel);
    if ((k < 32 && ((g_script.create_fail >> k) & 1)) || g_newsnd >= N_NEWSND) { log_word(0); return 0; }
    uint8_t* s = AR(A_NEWSND + 0x40 * g_newsnd++);
    memset(s, 0, 0x40);
    *(uint8_t**)s = AR(A_SVT);
    *(float*)(s + 4) = 1.0f;
    *(float*)(s + 8) = 1.0f;
    *(uint32_t*)(s + 0xc) = 0x5a5a;
    log_word((uint32_t)(s - g_arena));
    return s;
}
static void* __fastcall stub_sound_vdel(void* self, int, unsigned flags) { log_word('SDEL'); log_word((uint32_t)self); log_word(flags); return self; }

// the game's CRT must never put up a modal box: its fatal paths end the test quietly
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }
static int __cdecl stub_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}

// ---- building the world -----------------------------------------------------------------------------------------
static void rnd_bytes(uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)rnd(); }
static void rnd_floats(uint8_t* p, uint32_t n, float lo, float hi) { for (uint32_t i = 0; i + 4 <= n; i += 4) { float f = rf(lo, hi); memcpy(p + i, &f, 4); } }

static const char* k_names[] = {"viper", "vipergt", "cobra", "corvette", "prowler", "a", "", "indyjeep", "willys",
                                "abcdefghijklmnopqrstuvwxyz", "abcdefghijklmnopqrstuvwxyz0123"};

static int g_car_index;
static void random_car() {
    uint8_t* c = AR(A_CAR);
    rnd_floats(c, CAR_BYTES, -10.0f, 10.0f);
    *(uint32_t*)c = 0x004dbfd0;                                     // Car's vtable: +0x44 / +0x48 as the game has them
    g_car_index = chance(90) ? (int)(rnd() % 8) : (int)(rnd() % 16);
    *(int32_t*)(c + 0x510) = g_car_index;
    memset(c + 0x514, 0, 32);
    const char* nm = k_names[rnd() % (sizeof k_names / sizeof *k_names)];
    memcpy(c + 0x514, nm, strlen(nm));
    // the perceived RPM (+0xd70), around 1600 and the bands' corners
    float rpm;
    switch (rnd() % 6) {
    case 0: rpm = range(0.0f, 1600.0f); break;
    case 1: rpm = bitsf(0x44c80000u + (int32_t)(rnd() % 5) - 2); break;   // 1600 and its neighbours
    case 2: rpm = (float)(int)(rnd() % 8000); break;
    default: rpm = range(0.0f, 9000.0f); break;
    }
    if (chance(4)) rpm = wildf();
    *(float*)(c + 0xd70) = rpm;
    // the throttle (+0xc34 live, +0xde0 in a replay), around 0.1
    static const uint32_t k_thr_offs[2] = {0xc34u, 0xde0u};
    for (uint32_t off : k_thr_offs) {
        float t = chance(20) ? bitsf(0x3dcccccdu + (int32_t)(rnd() % 3) - 1) : chance(30) ? range(0.0f, 0.2f) : range(0.0f, 1.0f);
        if (chance(4)) t = wildf();
        *(float*)(c + off) = t;
    }
    *(uint8_t*)X_REPLAY_PLAY = chance(20);
    *(int32_t*)X_FOCUS = chance(50) ? g_car_index : (int32_t)(rnd() % 8);
    *(int32_t*)X_PLAYER = chance(50) ? g_car_index : (int32_t)(rnd() % 8);
    *(int32_t*)X_QUALITY = (int32_t)(rnd() % 3);
}

static void random_sound(uint8_t* s) {
    rnd_bytes(s, 0x40);
    *(uint8_t**)s = AR(A_SVT);
    float p = chance(20) ? 1.0f : rf(0.0f, 3.0f);
    float v = chance(15) ? 0.25f : chance(15) ? 0.0f : chance(5) ? -0.0f : rf(0.0f, 1.0f);
    memcpy(s + 4, &p, 4);
    memcpy(s + 8, &v, 4);
    for (int i = 0x29; i <= 0x2d; i++) s[i] = chance(50) ? (uint8_t)(rnd() & 1) : (uint8_t)rnd();
}

static void random_sample(uint8_t* p) {
    float* f = (float*)p;
    float a = range(0.0f, 2000.0f), b = a + range(0.0f, 3000.0f), c = b + range(0.0f, 4000.0f), d = c + range(0.0f, 3000.0f);
    if (chance(5)) b = a;                                               // zero-width ramps: a divide by zero
    if (chance(5)) c = b;
    if (chance(5)) d = c;
    if (chance(10)) { float t = a; a = c; c = t; }                      // out of order
    f[0] = a; f[1] = b; f[2] = c; f[3] = d;
    f[4] = rf(0.0f, 1.0f); f[5] = rf(0.0f, 1.0f); f[6] = chance(3) ? 0.0f : rf(500.0f, 8000.0f);
    for (int i = 0; i < 7; i++) if (chance(2)) wild_at(&f[i]);
}
// an RPM for a sample: on and around its corners
static float rpm_for(const uint8_t* p) {
    const float* f = (const float*)p;
    const int k = rnd() % 8;
    if (k < 4) { float x = f[k]; uint32_t u = fbits(x) + (int32_t)(rnd() % 3) - 1; return chance(50) ? x : bitsf(u); }
    if (k == 4) return rf(-100.0f, 10000.0f, 10);
    return range(f[0] - 100.0f, f[3] + 100.0f);
}

static void random_engine_sound() {
    uint8_t* e = AR(A_ES);
    rnd_bytes(e, 0x300);
    *(uint8_t**)(e + 8) = AR(A_CAR);
    *(int32_t*)(e + 0xc) = chance(90) ? g_car_index : (int32_t)(rnd() % 8);
    for (int i = 0; i < 7; i++) random_sample(e + 0x10 + 0x1c * i);
    const int null_pct = chance(20) ? 15 : 0;                          // a NULL band: faults where it is switched off
    for (int i = 0; i < 7; i++) *(uint8_t**)(e + 0xd4 + 4 * i) = chance(null_pct) ? 0 : snd(i);
    *(uint8_t**)(e + 0xf0) = chance(80) ? snd(7) : 0;
    *(int32_t*)(e + 0xf4) = chance(2) ? 0 : chance(3) ? 8 : (int32_t)(rnd() % 8);
}

static void random_wheel(uint8_t* w) {
    rnd_floats(w, WHEEL_BYTES, -5.0f, 5.0f);
    float slide = chance(20) ? 0.0f : chance(5) ? -0.0f : chance(10) ? range(-1.0f, 0.0f) : chance(40) ? range(0.0f, 1.0f) : range(0.0f, 20.0f);
    if (chance(4)) slide = wildf();
    memcpy(w + 0x11c, &slide, 4);
    w[0x121] = chance(70) ? 0 : (uint8_t)(chance(50) ? rnd() % 4 : rnd());
    for (int k = 0; k < 3; k++) { float c = rf(-800.0f, 800.0f); memcpy(w + 0xc8 + 4 * k, &c, 4); }
}

// six tyre sounds: 0 a RealTireSound (the one Update / the destructors run on), the rest Real or Dummy
static void random_tire_sounds() {
    for (int i = 0; i < 6; i++) {
        uint8_t* t = ts(i);
        rnd_bytes(t, 0x40);
        const bool real = i == 0 || chance(40);
        *(uint32_t*)t = real ? VT_RealTireSound : VT_DummyTireSound;
        *(int32_t*)(t + 4) = (int32_t)(rnd() % 4);
        *(uint8_t**)(t + 8) = wheel(rnd() % 4);
        if (real) {
            for (int k = 0; k < 4; k++) {
                const int r = rnd() % 8;
                *(uint8_t**)(t + 0xc + 4 * k) = r < 2 ? 0 : r == 2 ? t : ts(rnd() % 6);
            }
            rnd_floats(t + 0x1c, 12, -500.0f, 500.0f);
            *(int32_t*)(t + 0x28) = g_car_index;
            *(uint8_t**)(t + 0x30) = AR(A_CAR);
            *(uint8_t**)(t + 0x34) = chance(90) ? snd(8 + i) : 0;
            const uint32_t gains[4] = {0, 0x3f333333, 0x3f2147ae, 0x3f0ccccd};
            *(uint32_t*)(t + 0x38) = chance(90) ? gains[rnd() % 4] : fbits(rf(0.0f, 2.0f, 30));
        } else {
            t[0xc] = chance(80) ? 1 : (uint8_t)rnd();
        }
    }
}

static void random_resource() {
    uint8_t* r = AR(A_RES);
    const int32_t n = chance(5) ? 0 : chance(8) ? 8 + (int32_t)(rnd() % 2) : 1 + (int32_t)(rnd() % 7);
    *(int32_t*)r = n;
    for (int i = 0; i < 12; i++) {
        float* f = (float*)(r + 4 + 0x1c * i);
        uint8_t tmp[0x1c];
        random_sample(tmp);
        const float* s = (const float*)tmp;
        f[0] = s[6]; f[1] = s[4]; f[2] = s[5]; f[3] = s[0]; f[4] = s[1]; f[5] = s[2]; f[6] = s[3];
    }
}

static void random_lines() {
    g_script.file = chance(80) ? 7 : 0;
    g_script.nlines = (int)(rnd() % 10);
    for (int i = 0; i < 10; i++) {
        char* l = g_script.lines[i];
        const int k = rnd() % 10;
        if (k < 6) sprintf(l, "%g %g %g %g %g %g %g\r", range(1000, 5000), range(0, 1), range(0, 1), range(0, 1500),
                           range(1500, 3000), range(3000, 6000), range(6000, 8000));
        else if (k == 6) sprintf(l, "%g %g %g", range(0, 9), range(0, 9), range(0, 9));        // too few
        else if (k == 7) strcpy(l, "");
        else if (k == 8) strcpy(l, "# engine bands");
        else sprintf(l, "%g %g %g %g %g %g %g %g", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0);   // one more than it reads
        g_script.line_ok[i] = chance(95);
    }
}

// the tyre-sound table (16 cars x 4 wheels): the tyre sounds, NULLs, garbage
static void random_tire_table() {
    uint32_t* t = (uint32_t*)X_TIRE_TABLE;
    for (int i = 0; i < 64; i++) {
        const int k = rnd() % 4;
        t[i] = k == 0 ? 0 : k == 1 ? rnd() : (uint32_t)(uintptr_t)ts(rnd() % 6);
    }
}

// ---- the kinds of world ------------------------------------------------------------------------------------------
enum Kind {
    K_SAMPLE_CTOR, K_GETVOL, K_SAMPLE_UPDATE, K_ES_CTOR, K_ES_DTOR, K_ES_UPDATE,
    K_TS_CTOR, K_TS_CREATE, K_RTS_CTOR, K_RTS_DTOR, K_RTS_UPDATE, K_TS_OK, K_DTS_OK, K_DTS_UPDATE,
    K_TS_VDTOR, K_DTS_SDTOR, K_RTS_VDTOR, K_STATIC_INIT, N_KINDS
};
static const char* kind_names[N_KINDS] = {
    "EngineSoundSample::EngineSoundSample", "EngineSoundSample::GetVolume", "EngineSoundSample::Update",
    "EngineSound::EngineSound", "EngineSound::~EngineSound", "EngineSound::Update", "TireSound::TireSound",
    "TireSound::Create", "RealTireSound::RealTireSound", "RealTireSound::~RealTireSound", "RealTireSound::Update",
    "TireSound::Ok", "DummyTireSound::Ok", "DummyTireSound::Update", "TireSound::`vector deleting destructor'",
    "DummyTireSound::`scalar deleting destructor'", "RealTireSound::`vector deleting destructor'", "the $E initialisers",
};
static int kind_weight(Kind k) {
    switch (k) {
    case K_GETVOL: case K_SAMPLE_UPDATE: case K_ES_UPDATE: case K_RTS_UPDATE: return 12;
    case K_ES_CTOR: case K_TS_CREATE: return 8;
    default: return 2;
    }
}
static bool returns_byte(Kind k) { return k == K_TS_OK || k == K_DTS_OK; }

struct Args { float f0, f1; int i0, i1, i2; uint8_t* self; uint8_t* p; uint32_t u; const SndReg* e; };
static Args g_args;
static const SndReg* g_statics[80];
static int g_nstatics;

static void random_args(Kind k) {
    Args& a = g_args;
    memset(&a, 0, sizeof a);
    switch (k) {
    case K_SAMPLE_CTOR: a.self = AR(A_SAMPLE); rnd_bytes(a.self, 0x1c); break;
    case K_GETVOL: a.self = AR(A_SAMPLE); random_sample(a.self); a.f0 = rpm_for(a.self); break;
    case K_SAMPLE_UPDATE:
        a.self = AR(A_SAMPLE); random_sample(a.self); a.f0 = rpm_for(a.self);
        a.f1 = chance(10) ? 0.0f : rf(0.0f, 2.0f, 5);
        a.p = snd(0);
        if (chance(15)) {                                               // the sound already at what it will get
            const double v = ((EngineSoundSample_GetVolume)((EngineSoundSample*)a.self, 0, a.f0)) * a.f1;
            float v6 = (float)((double)(float)v * bitsf(0x3f19999a));
            memcpy(a.p + 8, &v6, 4);
            float pitch = (float)((double)a.f0 / *(float*)(a.self + 0x18));
            if (chance(50)) memcpy(a.p + 4, &pitch, 4);
        }
        break;
    case K_ES_CTOR:
        a.self = AR(A_ES); rnd_bytes(a.self, 0x300);
        g_script.res_try = chance(50); g_script.res_get = chance(70);
        random_resource(); random_lines();
        g_script.exists = rnd();
        g_script.create_fail = chance(70) ? 0 : (1u << (rnd() % 12));
        break;
    case K_ES_DTOR: case K_ES_UPDATE: a.self = AR(A_ES); a.u = chance(20) ? (uint8_t)(chance(50) ? 1 : rnd()) : 0; break;
    case K_TS_CTOR: a.self = ts(1); rnd_bytes(a.self, 0x40); a.i0 = (int)(rnd() % 4); a.p = wheel(a.i0); break;
    case K_TS_CREATE:
        a.i0 = chance(95) ? g_car_index : (int)(rnd() % 16);
        a.i1 = (int)(rnd() % 4);
        a.p = wheel(a.i1);
        g_script.alloc_fail = chance(3) ? 1u : 0u;
        g_script.create_fail = chance(10) ? 1u : 0u;
        random_tire_table();
        break;
    case K_RTS_CTOR:
        a.self = ts(1); rnd_bytes(a.self, 0x40);
        a.i0 = g_car_index; a.i1 = (int)(rnd() % 4); a.p = wheel(a.i1);
        a.i2 = chance(90) ? (int)(rnd() % 4) : chance(50) ? 4 + (int)(rnd() % 4) : -1;
        g_script.create_fail = chance(10) ? 1u : 0u;
        break;
    case K_RTS_DTOR: case K_RTS_UPDATE: case K_RTS_VDTOR: a.self = ts(0); a.u = rnd() % 3; break;
    case K_TS_OK: a.self = ts(rnd() % 6); break;
    case K_DTS_OK: case K_DTS_UPDATE: case K_DTS_SDTOR: case K_TS_VDTOR:
        a.self = ts(1 + rnd() % 5); a.u = rnd() % 3; break;
    case K_STATIC_INIT: a.e = g_statics[rnd() % g_nstatics]; break;
    default: break;
    }
}

// ---- running one pass ---------------------------------------------------------------------------------------------
typedef void*(__fastcall* Self_t)(void*, int);
typedef void*(__fastcall* SelfU_t)(void*, int, unsigned);
static uint64_t call_kind(Kind k, bool rw) {
    const Args& a = g_args;
    uint64_t r = 0;
    switch (k) {
    case K_SAMPLE_CTOR:
        r = (uint32_t)(rw ? (void*)EngineSoundSample_ctor((EngineSoundSample*)a.self, 0) : ((Self_t)0x00472db0)(a.self, 0));
        break;
    case K_GETVOL: {
        double d = rw ? EngineSoundSample_GetVolume((EngineSoundSample*)a.self, 0, a.f0)
                      : ((double(__fastcall*)(void*, int, float))0x00472df0)(a.self, 0, a.f0);
        memcpy(&r, &d, 8);
        break;
    }
    case K_SAMPLE_UPDATE:
        if (rw) EngineSoundSample_Update((EngineSoundSample*)a.self, 0, a.f0, (Sound3D*)a.p, a.f1);
        else ((void(__fastcall*)(void*, int, float, void*, float))0x00472ed0)(a.self, 0, a.f0, a.p, a.f1);
        break;
    case K_ES_CTOR:
        r = (uint32_t)(rw ? (void*)EngineSound_ctor((EngineSound*)a.self, 0, AR(A_CAR))
                          : ((void*(__fastcall*)(void*, int, void*))0x004728f0)(a.self, 0, AR(A_CAR)));
        break;
    case K_ES_DTOR:
        if (rw) EngineSound_dtor((EngineSound*)a.self, 0); else ((Self_t)0x00472c10)(a.self, 0);
        break;
    case K_ES_UPDATE:
        if (rw) EngineSound_Update((EngineSound*)a.self, 0, (uint8_t)a.u);
        else ((void(__fastcall*)(void*, int, uint8_t))0x00472c60)(a.self, 0, (uint8_t)a.u);
        break;
    case K_TS_CTOR:
        r = (uint32_t)(rw ? (void*)TireSound_ctor((TireSound*)a.self, 0, a.i0, a.p)
                          : ((void*(__fastcall*)(void*, int, int, void*))0x004731a0)(a.self, 0, a.i0, a.p));
        break;
    case K_TS_CREATE:
        r = (uint32_t)(rw ? (void*)TireSound_Create(a.i0, a.i1, a.p, AR(A_CAR))
                          : ((void*(__cdecl*)(int, int, void*, void*))0x004731c0)(a.i0, a.i1, a.p, AR(A_CAR)));
        break;
    case K_RTS_CTOR:
        r = (uint32_t)(rw ? (void*)RealTireSound_ctor((RealTireSound*)a.self, 0, a.i0, a.i1, a.p, AR(A_CAR), a.i2)
                          : ((void*(__fastcall*)(void*, int, int, int, void*, void*, int))0x004732f0)(a.self, 0, a.i0, a.i1, a.p, AR(A_CAR), a.i2));
        break;
    case K_RTS_DTOR:
        if (rw) RealTireSound_dtor((RealTireSound*)a.self, 0); else ((Self_t)0x004733c0)(a.self, 0);
        break;
    case K_RTS_UPDATE:
        if (rw) RealTireSound_Update((RealTireSound*)a.self, 0); else ((Self_t)0x004733f0)(a.self, 0);
        break;
    case K_TS_OK:
        r = rw ? TireSound_Ok((TireSound*)a.self, 0) : ((uint8_t(__fastcall*)(void*, int))0x00473670)(a.self, 0);
        break;
    case K_DTS_OK:
        r = rw ? DummyTireSound_Ok((DummyTireSound*)a.self, 0) : ((uint8_t(__fastcall*)(void*, int))0x004736a0)(a.self, 0);
        break;
    case K_DTS_UPDATE:
        if (rw) DummyTireSound_Update((DummyTireSound*)a.self, 0); else ((Self_t)0x004736b0)(a.self, 0);
        break;
    case K_TS_VDTOR:
        r = (uint32_t)(rw ? TireSound_vdtor((TireSound*)a.self, 0, a.u) : ((SelfU_t)0x00473680)(a.self, 0, a.u));
        break;
    case K_DTS_SDTOR:
        r = (uint32_t)(rw ? DummyTireSound_sdtor((DummyTireSound*)a.self, 0, a.u) : ((SelfU_t)0x004736c0)(a.self, 0, a.u));
        break;
    case K_RTS_VDTOR:
        r = (uint32_t)(rw ? RealTireSound_vdtor((RealTireSound*)a.self, 0, a.u) : ((SelfU_t)0x004736f0)(a.self, 0, a.u));
        break;
    case K_STATIC_INIT:
        ((void(__cdecl*)())(rw ? a.e->fn : (void*)(uintptr_t)a.e->v10))();
        break;
    default: break;
    }
    return r;
}

static void footprint_of(Kind k, Footprint& fp) {
    fp.n = 0;
    fp.replay_only = 0;
    fp.pure = false;
    const Args& g = g_args;
    switch (k) {
    case K_SAMPLE_CTOR: fpof_EngineSoundSample_ctor(fp, (EngineSoundSample*)g.self, 0); break;
    case K_GETVOL: fpof_EngineSoundSample_GetVolume(fp, (EngineSoundSample*)g.self, 0, g.f0); break;
    case K_SAMPLE_UPDATE: fpof_EngineSoundSample_Update(fp, (EngineSoundSample*)g.self, 0, g.f0, (Sound3D*)g.p, g.f1); break;
    case K_ES_CTOR: fpof_EngineSound_ctor(fp, (EngineSound*)g.self, 0, AR(A_CAR)); break;
    case K_ES_DTOR: fpof_EngineSound_dtor(fp, (EngineSound*)g.self, 0); break;
    case K_ES_UPDATE: fpof_EngineSound_Update(fp, (EngineSound*)g.self, 0, (uint8_t)g.u); break;
    case K_TS_CTOR: fpof_TireSound_ctor(fp, (TireSound*)g.self, 0, g.i0, g.p); break;
    case K_TS_CREATE: fpof_TireSound_Create(fp, g.i0, g.i1, g.p, AR(A_CAR)); break;
    case K_RTS_CTOR: fpof_RealTireSound_ctor(fp, (RealTireSound*)g.self, 0, g.i0, g.i1, g.p, AR(A_CAR), g.i2); break;
    case K_RTS_DTOR: fpof_RealTireSound_dtor(fp, (RealTireSound*)g.self, 0); break;
    case K_RTS_UPDATE: fpof_RealTireSound_Update(fp, (RealTireSound*)g.self, 0); break;
    case K_TS_OK: fpof_TireSound_Ok(fp, (TireSound*)g.self, 0); break;
    case K_DTS_OK: fpof_DummyTireSound_Ok(fp, (DummyTireSound*)g.self, 0); break;
    case K_DTS_UPDATE: fpof_DummyTireSound_Update(fp, (DummyTireSound*)g.self, 0); break;
    case K_TS_VDTOR: fpof_TireSound_vdtor(fp, (TireSound*)g.self, 0, g.u); break;
    case K_DTS_SDTOR: fpof_DummyTireSound_sdtor(fp, (DummyTireSound*)g.self, 0, g.u); break;
    case K_RTS_VDTOR: fpof_RealTireSound_vdtor(fp, (RealTireSound*)g.self, 0, g.u); break;
    case K_STATIC_INIT: ((void (*)(Footprint&))g.e->fp)(fp); break;
    default: fp.replay_only = "?"; break;
    }
}
static bool in_footprint(const Footprint& fp, const uint8_t* p) {
    for (int i = 0; i < fp.n; i++)
        if (p >= (const uint8_t*)fp.r[i].p && p < (const uint8_t*)fp.r[i].p + fp.r[i].n) return true;
    return false;
}

static uint32_t g_fault_eip, g_fault_code, g_fault_addr;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_eip = (uint32_t)e->ContextRecord->Eip;
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_addr = e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static unsigned g_pc;
static int g_unmask;                // 0 masked; 1 overflow + divide-by-zero; 2 those and the denormal operand
static int run_guarded(Kind k, bool rw, uint64_t* ret) {
    g_log.n = 0;
    g_heap = AR(A_HEAP);
    g_alloc_calls = g_create_calls = g_exists_calls = g_line_calls = g_newsnd = 0;
    unsigned cw;
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    if (g_unmask == 1) _controlfp_s(&cw, _MCW_EM & ~(_EM_OVERFLOW | _EM_ZERODIVIDE), _MCW_EM);
    if (g_unmask == 2) _controlfp_s(&cw, _MCW_EM & ~(_EM_OVERFLOW | _EM_ZERODIVIDE | _EM_DENORMAL), _MCW_EM);
    int fault = 0;
    __try {
        *ret = call_kind(k, rw);
        __asm fwait
    } __except (fault_filter(GetExceptionInformation())) { fault = 1; }
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    return fault;
}

static const char* where(uint32_t a, char* buf) {
    uint32_t base = (uint32_t)(uintptr_t)g_arena;
    if (a >= base && a < base + ARENA_BYTES) { sprintf(buf, "arena+0x%x", a - base); return buf; }
    sprintf(buf, "0x%08x", a);
    return buf;
}

// ---- main ----------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 200000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_snd_car.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);         // _amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);         // _NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);         // _FF_MSGBANNER
    patch_jmp(0x004d5c10, (void*)&stub_crt_fatal);         // _fptrap
    patch_jmp(0x004d6850, (void*)&stub_crt_msgbox);        // __crtMessageBoxA
    ((void(__cdecl*)())0x004ce150)();                      // the CRT's floating point, as its start-up leaves it
    patch_jmp(0x004140e0, (void*)&stub_MemAlloc);
    patch_jmp(0x00414390, (void*)&stub_delete);
    patch_jmp(0x00411150, (void*)&stub_log);
    patch_jmp(0x004112b0, (void*)&stub_panic);
    patch_jmp(0x004cf0a0, (void*)&stub_sprintf);
    patch_jmp(0x004ce190, (void*)&stub_sscanf);
    patch_jmp(0x00419ce0, (void*)&stub_resource_try);
    patch_jmp(0x00419fa0, (void*)&stub_resource_get);
    patch_jmp(0x00419d10, (void*)&stub_resource_exists);
    patch_jmp(0x0041a450, (void*)&stub_resource_forget);
    patch_jmp(0x00411780, (void*)&stub_file_open);
    patch_jmp(0x004119b0, (void*)&stub_file_read_line);
    patch_jmp(0x00411850, (void*)&stub_file_close);
    patch_jmp(0x00472560, (void*)&stub_sound3d_create);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_arena_snap = (uint8_t*)malloc(ARENA_BYTES);
    g_arena_after = (uint8_t*)malloc(ARENA_BYTES);
    g_data_snap = (uint8_t*)malloc(DATA_BYTES);
    g_data_after = (uint8_t*)malloc(DATA_BYTES);

    int nreg = 0;
    for (SndReg* r = SndReg::head(); r; r = r->next) {
        nreg++;
        if (r->name[0] == '$') g_statics[g_nstatics++] = r;
    }
    printf("%d rewrites registered (%d $E initialisers)\n", nreg, g_nstatics);

    static Footprint fp;
    int fault_log_diff = 0, fault_state_diff = 0;
    int differ = 0, faults = 0, fault_both = 0, fp_bad = 0, replay_only = 0, wild_runs = 0, unmask_runs[3] = {0};
    int per[N_KINDS] = {0}, per_bad[N_KINDS] = {0}, per_fault[N_KINDS] = {0};
    // coverage, from the original's results
    int c_vol_silent = 0, c_vol_up = 0, c_vol_mid = 0, c_vol_down = 0, c_upd_off = 0, c_upd_on = 0, c_upd_same = 0;
    int c_idle_on = 0, c_idle_off = 0, c_band_calls = 0, c_band_mute = 0, c_ctor_res = 0, c_ctor_get = 0, c_ctor_txt = 0,
        c_ctor_nofile = 0, c_ctor_overflow = 0, c_create_real = 0, c_create_dummy = 0, c_tire_none = 0, c_tire_some = 0,
        c_panic = 0;
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weight((Kind)i);
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), kk = 0;
        while (pick >= kind_weight((Kind)kk)) pick -= kind_weight((Kind)kk++);
        const Kind kind = (Kind)kk;
        memset(&g_script, 0, sizeof g_script);
        memset(g_arena, 0, ARENA_BYTES);
        random_car();
        for (int i = 0; i < 16; i++) random_sound(snd(i));
        void** svt = (void**)AR(A_SVT);
        svt[0] = (void*)&stub_sound_vdel;
        for (int i = 1; i < 8; i++) svt[i] = (void*)0x00000bad;
        random_engine_sound();
        for (int i = 0; i < 4; i++) random_wheel(wheel(i));
        random_tire_sounds();
        random_args(kind);
        const bool wild = chance(10);
        if (wild) {                                                       // floats replaced wholesale (not the pointers)
            wild_runs++;
            for (int i = 0; i < 7 * 7; i++) if (chance(30)) wild_at(AR(A_ES) + 0x10 + 4 * i);
            for (int i = 0; i < 7; i++) if (chance(30)) wild_at(AR(A_SAMPLE) + 4 * i);
            for (int i = 0; i < 16; i++) { if (chance(30)) wild_at(snd(i) + 4); if (chance(30)) wild_at(snd(i) + 8); }
            for (int i = 0; i < 4; i++) { if (chance(40)) wild_at(wheel(i) + 0x11c); for (int k = 0; k < 3; k++) if (chance(20)) wild_at(wheel(i) + 0xc8 + 4 * k); }
            for (int i = 0; i < 6; i++) if (*(uint32_t*)ts(i) == VT_RealTireSound && chance(40)) wild_at(ts(i) + 0x38);
            if (chance(30)) wild_at(AR(A_CAR) + 0xd70);
            if (chance(30)) wild_at(AR(A_CAR) + 0xc34);
            if (chance(30)) g_args.f0 = wildf();
            if (chance(30)) g_args.f1 = wildf();
        }
        g_pc = chance(50) ? _PC_53 : _PC_24;
        g_unmask = chance(30) ? 1 : chance(14) ? 2 : 0;
        unmask_runs[g_unmask]++;
        footprint_of(kind, fp);
        if (fp.replay_only) replay_only++;
        memcpy(g_arena_snap, g_arena, ARENA_BYTES);
        memcpy(g_data_snap, DATA, DATA_BYTES);
        uint64_t ro = 0, rn = 0;
        int fo = run_guarded(kind, false, &ro);
        const uint32_t fo_code = g_fault_code, fo_eip = g_fault_eip;
        memcpy(g_arena_after, g_arena, ARENA_BYTES);
        memcpy(g_data_after, DATA, DATA_BYTES);
        g_log_orig = g_log;
        memcpy(g_arena, g_arena_snap, ARENA_BYTES);
        memcpy(DATA, g_data_snap, DATA_BYTES);
        int fn = run_guarded(kind, true, &rn);
        per[kind]++;
        const char* kname = kind == K_STATIC_INIT ? g_args.e->name : kind_names[kind];
        if (fo || fn) {
            faults++;
            per_fault[kind]++;
            if (fo && fn) {
                fault_both++;
                // both faulted: what each did up to its fault should agree too (the x87 reports a fault at its next FPU
                // instruction, so an integer store the rewrite's compiler moved could land on either side: counted apart)
                const bool log_same = g_log.n == g_log_orig.n &&
                                      !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
                if (!log_same) { fault_log_diff++; printf("  (world %d, %s: both faulted, the logs differ)\n", it, kname); }
                else if (memcmp(g_arena, g_arena_after, ARENA_BYTES) || memcmp(DATA, g_data_after, DATA_BYTES)) {
                    fault_state_diff++;
                    for (uint32_t i = 0; i < ARENA_BYTES; i += 4)
                        if (memcmp(g_arena_after + i, g_arena + i, 4)) {
                            char b2[64];
                            if (fault_state_diff <= 10)
                                printf("  (world %d, %s: both faulted, the state differs at %s, unmask %d)\n", it, kname,
                                       where((uint32_t)(uintptr_t)(g_arena + i), b2), g_unmask);
                            break;
                        }
                }
                if (fault_both <= 4) printf("  (world %d, %s: both faulted, %08x at %08x / %08x at %08x, pc %s, unmask %d)\n", it, kname,
                                             fo_code, fo_eip, g_fault_code, g_fault_eip, g_pc == _PC_24 ? "24" : "53", g_unmask);
            } else {
                printf("  world %d (%s): %s / %s; fault %08x at %08x (original's %08x at %08x), address %08x, pc %s, unmask %d\n", it,
                       kname, fo ? "original faulted" : "original ran", fn ? "rewrite faulted" : "rewrite ran", g_fault_code,
                       g_fault_eip, fo_code, fo_eip, g_fault_addr, g_pc == _PC_24 ? "24" : "53", g_unmask);
                differ++; per_bad[kind]++;
            }
            memcpy(g_arena, g_arena_snap, ARENA_BYTES);
            memcpy(DATA, g_data_snap, DATA_BYTES);
            continue;
        }
        if (returns_byte(kind)) { ro &= 0xff; rn &= 0xff; }
        bool same = true;
        char buf[64];
        for (uint32_t i = 0; i < ARENA_BYTES; i += 4)
            if (memcmp(g_arena_after + i, g_arena + i, 4)) {
                uint32_t x, y; memcpy(&x, g_arena_after + i, 4); memcpy(&y, g_arena + i, 4);
                if (same) printf("  MISMATCH world %d (%s)\n", it, kname);
                printf("    %s original %08x (%.9g) rewrite %08x (%.9g)\n", where((uint32_t)(uintptr_t)(g_arena + i), buf), x, bitsf(x), y, bitsf(y));
                same = false;
                break;
            }
        for (uint32_t i = 0; i < DATA_BYTES; i += 4)
            if (memcmp(g_data_after + i, DATA + i, 4)) {
                uint32_t x, y; memcpy(&x, g_data_after + i, 4); memcpy(&y, DATA + i, 4);
                if (same) printf("  MISMATCH world %d (%s)\n", it, kname);
                printf("    %s original %08x rewrite %08x\n", where(0x4e1000 + i, buf), x, y);
                same = false;
                break;
            }
        if (ro != rn) {
            if (same) printf("  MISMATCH world %d (%s)\n", it, kname);
            printf("    return: original %016llx rewrite %016llx\n", (unsigned long long)ro, (unsigned long long)rn);
            same = false;
        }
        if (g_log.n != g_log_orig.n || memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX))) {
            if (same) printf("  MISMATCH world %d (%s)\n", it, kname);
            printf("    the stub logs differ: %u / %u words\n", g_log_orig.n, g_log.n);
            uint32_t m = g_log.n < g_log_orig.n ? g_log.n : g_log_orig.n;
            if (m > LOG_MAX) m = LOG_MAX;
            for (uint32_t i = 0; i < m; i++) if (g_log.w[i] != g_log_orig.w[i]) { printf("    first at %u: %08x / %08x\n", i, g_log_orig.w[i], g_log.w[i]); break; }
            same = false;
        }
        if (!same) {
            differ++; per_bad[kind]++;
            printf("    (pc %s, wild %d, unmask %d)\n", g_pc == _PC_24 ? "24" : "53", wild, g_unmask);
            if (differ >= 30) { printf("stopping after 30 differing worlds\n"); break; }
        }
        // the original's writes against the footprint
        if (!fp.replay_only) {
            bool bad = false;
            for (uint32_t i = 0; i < ARENA_BYTES && !bad; i++)
                if (g_arena_after[i] != g_arena_snap[i] && !in_footprint(fp, g_arena + i)) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, kname, where((uint32_t)(uintptr_t)(g_arena + i), buf));
                    bad = true;
                }
            for (uint32_t i = 0; i < DATA_BYTES && !bad; i++)
                if (g_data_after[i] != g_data_snap[i] && !in_footprint(fp, DATA + i)) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, kname, where(0x4e1000 + i, buf));
                    bad = true;
                }
            fp_bad += bad;
        }
        // coverage
        auto logged = [&](uint32_t tag) { for (uint32_t i = 0; i < g_log_orig.n && i < LOG_MAX; i++) if (g_log_orig.w[i] == tag) return true; return false; };
        if (kind == K_GETVOL) {
            double d; memcpy(&d, &ro, 8);
            const float* f = (const float*)g_args.self;
            if (d == 0.0) c_vol_silent++;
            else if (g_args.f0 < f[1]) c_vol_up++;
            else if (g_args.f0 > f[2]) c_vol_down++;
            else c_vol_mid++;
        }
        if (kind == K_SAMPLE_UPDATE) {
            const uint8_t* s0 = g_arena_snap + (g_args.p - g_arena);
            const uint8_t* s1 = g_arena_after + (g_args.p - g_arena);
            if (s1[0x2a] == 1 && s1[0x29] == s0[0x29]) c_upd_off++;
            else if (!memcmp(s0 + 4, s1 + 4, 8)) c_upd_same++;
            else c_upd_on++;
        }
        if (kind == K_ES_UPDATE) {
            const uint8_t* e = g_arena_snap + A_ES;
            const uint8_t* idle = *(uint8_t* const*)(e + 0xf0);
            if (idle && *(const int32_t*)(e + 0xf4)) {
                const uint8_t* i1 = g_arena_after + (idle - g_arena);
                (i1[0x2a] ? c_idle_off : c_idle_on)++;
            }
            for (int i = 0; i < 7 && i < *(const int32_t*)(e + 0xf4); i++) {
                const uint8_t* b = *(uint8_t* const*)(e + 0xd4 + 4 * i);
                if (!b) continue;
                const uint8_t* b1 = g_arena_after + (b - g_arena);
                if (b1[0x2b] == 1 && b1[0x29] == 1 && b1[0x2a] == 0) c_band_calls++;
                else if (b1[0x2a] == 1) c_band_mute++;
            }
        }
        if (kind == K_ES_CTOR) {
            if (g_script.res_try) c_ctor_res++;
            else if (g_script.res_get) c_ctor_get++;
            else if (g_script.file) c_ctor_txt++;
            else c_ctor_nofile++;
            if ((g_script.res_try || g_script.res_get) && *(int32_t*)AR(A_RES) > 7 && *(int32_t*)(g_arena_after + A_ES + 0xf4) > 7)
                c_ctor_overflow++;
        }
        if (kind == K_TS_CREATE && ro) (*(uint32_t*)(uintptr_t)ro == VT_RealTireSound ? c_create_real : c_create_dummy)++;
        if (kind == K_RTS_UPDATE) {
            const uint8_t* t0 = g_arena_snap + A_TS, *t1 = g_arena_after + A_TS;
            (memcmp(t0 + 0x1c, t1 + 0x1c, 12) ? c_tire_some : c_tire_none)++;
        }
        if (logged('PANC')) c_panic++;
    }
    printf("%d worlds (%d wild; %d masked, %d with overflow / divide-by-zero unmasked, %d with the denormal operand too): %d differ, "
           "%d faulted (%d in both), %d changed bytes outside the footprint, %d replay-only\n",
           iterations, wild_runs, unmask_runs[0], unmask_runs[1], unmask_runs[2], differ, faults, fault_both, fp_bad, replay_only);
    printf("faulted in both: %d with differing stub logs, %d with differing state at the fault\n", fault_log_diff, fault_state_diff);
    printf("per function (worlds / faulted in both / differing):\n");
    for (int i = 0; i < N_KINDS; i++) printf("  %-46s %7d / %5d / %d\n", kind_names[i], per[i], per_fault[i], per_bad[i]);
    printf("GetVolume: silent %d, ramp up %d, plateau %d, ramp down %d; Sample::Update: off %d, changed %d, unchanged %d\n",
           c_vol_silent, c_vol_up, c_vol_mid, c_vol_down, c_upd_off, c_upd_on, c_upd_same);
    printf("EngineSound::Update: idle on %d / off %d, bands updated %d / switched off %d; ctor: .ens %d, engine.ens %d, engine.txt %d, "
           "no file %d, count overflow %d\n", c_idle_on, c_idle_off, c_band_calls, c_band_mute, c_ctor_res, c_ctor_get, c_ctor_txt,
           c_ctor_nofile, c_ctor_overflow);
    printf("TireSound::Create: real %d, dummy %d; RealTireSound::Update squealing %d / none %d; LogPanic reached %d\n",
           c_create_real, c_create_dummy, c_tire_some, c_tire_none, c_panic);
    return differ || fp_bad ? 1 : 0;
}
