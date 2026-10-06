// world_wld_effects.cpp -- group W4's rewrites (hook/wld_effects.cpp: skid marks, shadows, reflections, smoke, debris,
// sparks and splashes) against the originals, on random worlds, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_wld_effects.cpp
//        /Fo<dir>\ /Fe<dir>\world_wld_effects.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_wld_effects.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. Every world
// starts from the game's own set-up, run by the ORIGINAL code: EffectsBegin (32 SplashObjects, the DebrisObject with
// its 16 SmokeObjects and their models, the SparksObject, the three event handlers, the world's object list) and a
// SkidObject of 0..40 marks, all on a bump heap inside the world. Then everything is randomised: the clock
// (physics_tick), the camera, the world's switches (FX, smoke, shadows, the mirror, the camera type and focus car),
// every splash (its volume, times, droplets and vertices), the debris and smoke (times, positions, velocities,
// models), the sparks, the skid marks and four SkidClients on them, a fake car (WorldObject: a vtable of loggers, a
// frame, the car index and level of detail), a ShadowObject and a ReflectionObject on it, a shadow model (random
// vertices and triangles, some degenerate) and the arguments (events, points, axes, names).
//
// Stubbed (a jump to a logger): MemAlloc (the bump heap, sometimes NULL where the caller checks), MemFree,
// operator delete, LogPanic, sprintf (the host's), Random (scripted), mrModelInfoGet (the random model, or none),
// mrModelInfoForget, mrModelCreate, mrModelBuild (logs the model it builds: header, vertices, materials, triangles),
// mrModelDestroy, mrModelLoad, mrModelUnload, gxGetTexture, gxForgetTexture, TextureGet, TextureForget. Everything
// else runs as the game's code: GraphObject's constructor and destructor, P3DBase's, WorldAddGob, the event table,
// PhysicsGetTime / WorldGetElapsedTime and the world's accessors, and this file's functions where another calls them.
//
// Each world picks a function (or a sequence: many frames of skids on and off, splash events and updates, crashes
// and scrapes with the smoke updating, shadows and reflections following the camera); the arena and the image's
// whole .data/.bss are saved, the original runs, the result is kept, the world restored, the rewrite runs; the arena,
// .data/.bss, the return value and the stubs' logs are compared, and every byte the original changed must lie in the
// rewrite's footprint unless it's replay_only (everything here runs on the main thread: no statics are exempt).
// The FPU runs at a random precision (24 or 53 bits), in 30% of the worlds with overflow and divide-by-zero unmasked
// (docs/PORTING.md 10a: a fault must come from the same store in both; each pass starts and ends with fninit, as a
// caught fault leaves the x87 stack full). A share of the worlds has floats replaced by extreme values (never the
// pointers). The game's CRT fatal paths and message box are stubbed and SetErrorMode is set: nothing modal.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif

#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#include "../hook/port.h"
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { printf("footprint: object() not expected (%s)\n", what); add(obj, 4, what); }
void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

#include "../hook/wld_effects.cpp"

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
    default: return bitsf(rnd());
    }
}
static void wild_at(void* p) { float f = wildf(); memcpy(p, &f, 4); }
static void rnd_p3(void* p, float lo, float hi) { float* f = (float*)p; for (int i = 0; i < 3; i++) f[i] = range(lo, hi); }

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
    ARENA_BYTES = 0x60000,
    A_HEAP = 0x00000, A_HEAP_END = 0x40000,  // MemAlloc's bump heap
    A_WOBJ = 0x40000,                        // the fake car (WorldObject), 0x300
    A_VTAB = 0x40400,                        // its vtable (16 slots)
    A_DETAIL = 0x40480,                      // its level of detail (the int +0x28c points at)
    A_SKIDOBJ = 0x40500,                     // the SkidObject (0x14)
    A_CLIENTS = 0x40600,                     // four SkidClients (0x40 each)
    A_SHADOW = 0x40800,                      // a ShadowObject (0x1c)
    A_REFL = 0x40880,                        // a ReflectionObject (0x1c)
    A_OBJ = 0x40a00,                         // a constructor's target (0x400)
    A_MODEL = 0x42000, A_MODEL_END = 0x48000,// mrModelInfoGet's model
    A_ARGS = 0x58000,                        // arguments
};
enum : uint32_t {
    M_VERTS = 0x100, M_TRIS = 0x3400, M_MATS = 0x5400, M_1C = 0x5500, M_24 = 0x5600,
    G_EVENT = 0x000, G_P3A = 0x100, G_P3B = 0x110, G_P3C = 0x120, G_M3 = 0x200, G_IDX = 0x300, G_TRI = 0x400, G_NAME = 0x600,
};
static uint8_t* g_arena;
static uint8_t* g_arena_snap;
static uint8_t* g_arena_after;
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* g_data_snap;
static uint8_t* g_data_after;
static uint8_t* AR(uint32_t off) { return g_arena + off; }
static uint8_t* ARG(uint32_t off) { return g_arena + A_ARGS + off; }

// ---- the stubs' log and script ------------------------------------------------------------------------------------
enum { LOG_MAX = 16384 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void log_word(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static void log_bytes(const void* p, int n) { for (int i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, (const uint8_t*)p + i, n - i < 4 ? n - i : 4); log_word(w); } }
static void log_str(const char* s) { size_t n = strlen(s); log_word((uint32_t)n); log_bytes(s, (int)n); }
static bool in_arena(const void* p, uint32_t n) { return (const uint8_t*)p >= g_arena && (const uint8_t*)p + n <= g_arena + ARENA_BYTES && n < ARENA_BYTES; }

struct Script {
    uint32_t rand_seed;
    uint32_t alloc_fail;           // bit per MemAlloc call: NULL
    uint8_t model_found;
    uint8_t wobj_visible;
    float wobj_z;
    int tex;
};
static Script g_script;
static uint8_t* g_heap;             // the bump pointer
static uint8_t* g_heap_mark;        // where a pass starts
static int g_alloc_calls;
static uint32_t g_rand;

static void* __cdecl stub_MemAlloc(int n) {
    log_word('MALC'); log_word((uint32_t)n);
    const int k = g_alloc_calls++;
    if (k < 32 && ((g_script.alloc_fail >> k) & 1)) { log_word(0); return 0; }
    if (n < 0 || (uint32_t)n > (uint32_t)(AR(A_HEAP_END) - g_heap)) { log_word(1); return 0; }
    uint8_t* p = g_heap;
    g_heap += ((uint32_t)n + 7) & ~7u;
    log_word((uint32_t)(p - g_arena));
    return p;
}
static void __cdecl stub_MemFree(void* p) { log_word('MFRE'); log_word((uint32_t)p); }
static void __cdecl stub_delete(void* p) { log_word('DELE'); log_word((uint32_t)p); }
static void __cdecl stub_panic(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    log_word('PANC'); log_word((uint32_t)fmt);
    if ((uint32_t)fmt == 0x004f4e24) { log_word(va_arg(ap, uint32_t)); log_word(va_arg(ap, uint32_t)); }
    va_end(ap);
}
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    log_word('SPRF'); log_word((uint32_t)fmt); log_str(buf);
    return r;
}
static int __cdecl stub_random(int n) {
    g_rand ^= g_rand << 13; g_rand ^= g_rand >> 17; g_rand ^= g_rand << 5;
    int v = 0;
    if (n > 0) {
        uint32_t k = g_rand % 64;
        v = k == 0 ? 0 : k == 1 ? n - 1 : k == 2 ? n : (int)(g_rand % (uint32_t)n);
    }
    log_word('RAND'); log_word((uint32_t)n); log_word((uint32_t)v);
    return v;
}
static void* __cdecl stub_info_get(const char* name) {
    log_word('MIGT'); log_str(name);
    return g_script.model_found ? AR(A_MODEL) : 0;
}
static void __cdecl stub_info_forget(void* p) { log_word('MIFG'); log_word((uint32_t)p); }
static int g_model_next;
static int __cdecl stub_model_create(const char* name, int n) { log_word('MCRE'); log_str(name); log_word((uint32_t)n); return 0x100 + g_model_next++; }
static void __cdecl stub_model_build(int model, const uint8_t* info, int flags) {
    log_word('MBLD'); log_word((uint32_t)model); log_word((uint32_t)info); log_word((uint32_t)flags);
    if (!in_arena(info, 0x28)) return;
    log_bytes(info, 0x28);
    const int32_t* h = (const int32_t*)info;
    struct { int n, size; uint32_t ptr; } parts[5] = {{h[0], 32, (uint32_t)h[1]}, {h[2], 32, (uint32_t)h[3]}, {h[4], 8, (uint32_t)h[5]},
                                                      {h[6], 16, (uint32_t)h[7]}, {h[8], 4, (uint32_t)h[9]}};
    for (auto& p : parts) {
        if (p.n <= 0 || p.n > 4096 || !in_arena((void*)p.ptr, (uint32_t)(p.n * p.size))) continue;
        log_bytes((void*)p.ptr, p.n * p.size);
    }
}
static void __cdecl stub_model_destroy(int m) { log_word('MDES'); log_word((uint32_t)m); }
static int __cdecl stub_model_load(const char* name) { log_word('MLOD'); log_str(name); return 0x200 + g_model_next++; }
static void __cdecl stub_model_unload(int m) { log_word('MUNL'); log_word((uint32_t)m); }
static int __cdecl stub_gx_get_texture(const char* name, int k) { log_word('GXGT'); log_str(name); log_word((uint32_t)k); return g_script.tex++; }
static void __cdecl stub_gx_forget(int t) { log_word('GXFG'); log_word((uint32_t)t); }
static int __cdecl stub_texture_get(const char* name, int k) { log_word('TXGT'); log_str(name); log_word((uint32_t)k); return g_script.tex++; }
static void __cdecl stub_texture_forget(int t) { log_word('TXFG'); log_word((uint32_t)t); }
// the fake car's virtuals
static uint8_t __fastcall stub_wobj_visible(void* self, int) { log_word('WVIS'); log_word((uint32_t)self); return g_script.wobj_visible; }
static float __fastcall stub_wobj_getz(void* self, int) { log_word('WGTZ'); log_word((uint32_t)self); return g_script.wobj_z; }

// the game's CRT must never put up a modal box: its fatal paths end the test quietly
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }
static int __cdecl stub_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}

// ---- the image's statics --------------------------------------------------------------------------------------
enum : uint32_t {
    X_TICK = 0x0052161c, X_FX = 0x0055404c, X_SMOKE_ON = 0x00522a1c, X_SMOKE = 0x0055336c, X_SHADOW = 0x005522c8,
    X_FOCUS = 0x004f4358, X_CAMTYPE = 0x0055335c, X_MIRROR = 0x004f4c9c, X_PAUSED = 0x00521608, X_CAMERA = 0x00553328,
    X_NUM_EVENTS = 0x00557f60,
};
static float now_time() { return (float)(*(int32_t*)X_TICK * (double)bitsf(0x3c83126f)); }
static uint8_t* splash(int i) { return *(uint8_t**)(S_SPLASH_TAB + 4 * i); }
static uint8_t* debris() { return *(uint8_t**)S_DEBRIS; }
static uint8_t* sparks() { return *(uint8_t**)S_SPARKS; }
static uint8_t* smoke(int i) { return *(uint8_t**)(debris() + 0xd4 + 4 * i); }

// ---- building the world -----------------------------------------------------------------------------------------
typedef void(__cdecl* Void_t2)();
typedef void*(__fastcall* SkidCtor_t)(void*, int, int);
static bool g_setup_ok;
static void setup_world() {
    memset(g_arena, 0, ARENA_BYTES);
    memset(AR(A_HEAP), 0xcd, A_HEAP_END - A_HEAP);
    *(int32_t*)X_NUM_EVENTS = 0;
    *(int32_t*)S_NUM_GOBS = 0;
    *(int32_t*)S_SPLASH_COUNT = 0;
    memset((void*)S_SPLASH_TAB, 0, 0x80);
    *(uint32_t*)S_DEBRIS = 0;
    *(uint32_t*)S_SPARKS = 0;
    g_heap = AR(A_HEAP);
    g_alloc_calls = 0;
    g_script.alloc_fail = 0;
    g_script.tex = 1;
    g_model_next = 0;
    g_rand = rnd() | 1;
    unsigned cw;
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
#else
    __asm fninit
#endif
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    g_setup_ok = true;
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] {
        ((Void_t2)0x00467460)();                                         // EffectsBegin, the original
        ((SkidCtor_t)0x004687d0)(AR(A_SKIDOBJ), 0, chance(5) ? 0 : 1 + (int)(rnd() % 40));
    })) { g_setup_ok = false; }
#else
    __try {
        ((Void_t2)0x00467460)();                                         // EffectsBegin, the original
        ((SkidCtor_t)0x004687d0)(AR(A_SKIDOBJ), 0, chance(5) ? 0 : 1 + (int)(rnd() % 40));
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_setup_ok = false; }
#endif
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    g_heap_mark = g_heap;
}

static void random_frame(float* f, float spread) {                        // a rotation (roughly) and a position
    for (int i = 0; i < 9; i++) f[i] = range(-1.0f, 1.0f);
    f[0] = 1.0f; f[4] = 1.0f; f[8] = 1.0f;
    rnd_p3(f + 9, -spread, spread);
}
static const uint32_t k_volumes[4] = {0x0bad0010, 0x0bad0020, 0x0bad0030, 0x0bad0040};

static void random_model() {
    uint8_t* m = AR(A_MODEL);
    int32_t* h = (int32_t*)m;
    int nv = chance(80) ? 3 + (int)(rnd() % 60) : 3 + (int)(rnd() % 400);
    int nt = chance(80) ? 1 + (int)(rnd() % 80) : 200 + (int)(rnd() % 800);
    h[0] = nv; h[1] = (int32_t)(m + M_VERTS);
    h[2] = (int)(rnd() % 4); h[3] = (int32_t)(m + M_MATS);
    h[4] = nt; h[5] = (int32_t)(m + M_TRIS);
    h[6] = (int)(rnd() % 5); h[7] = (int32_t)(m + M_1C);
    h[8] = (int)(rnd() % 7); h[9] = (int32_t)(m + M_24);
    const bool flat = chance(40);
    for (int i = 0; i < nv; i++) {
        float* v = (float*)(m + M_VERTS + 32 * i);
        v[0] = range(-2.0f, 2.0f); v[1] = flat ? range(-0.05f, 0.05f) : range(-1.0f, 1.0f); v[2] = range(-2.0f, 2.0f);
        for (int k = 3; k < 8; k++) v[k] = range(-1.0f, 1.0f);
        if (chance(3)) wild_at(&v[rnd() % 3]);
    }
    int16_t* t = (int16_t*)(m + M_TRIS);
    for (int i = 0; i < nt; i++) {
        for (int k = 0; k < 3; k++) t[4 * i + k] = (int16_t)(rnd() % nv);
        if (chance(10)) t[4 * i + 1] = t[4 * i];                        // degenerate
        t[4 * i + 3] = (int16_t)rnd();
    }
    for (int i = 0; i < 0x100; i++) m[M_MATS + i] = (uint8_t)rnd();
    for (int i = 0; i < 0x100; i++) m[M_1C + i] = (uint8_t)rnd();
    for (int i = 0; i < 0x100; i++) m[M_24 + i] = (uint8_t)rnd();
}

static void random_world() {
    const int32_t tick = chance(10) ? (int32_t)(rnd() % 100) : (int32_t)(rnd() % 200000);
    *(int32_t*)X_TICK = tick;
    const float now = now_time();
    *(uint8_t*)X_FX = chance(85) ? 1 : (uint8_t)(chance(50) ? 0 : rnd());
    *(int32_t*)X_SMOKE_ON = chance(90);
    *(int32_t*)X_SMOKE = chance(85) ? 1 : (int32_t)(rnd() % 3);
    *(int32_t*)X_SHADOW = (int32_t)(rnd() % 4);
    *(int32_t*)X_FOCUS = (int32_t)(rnd() % 4);
    *(int32_t*)X_CAMTYPE = (int32_t)(rnd() % 16) - 2;
    *(uint8_t*)X_MIRROR = chance(15);
    *(uint8_t*)X_PAUSED = chance(15);
    g_script.model_found = chance(90);
    g_script.wobj_visible = chance(80) ? 1 : 0;
    g_script.wobj_z = range(-100.0f, 100.0f);
    g_script.rand_seed = rnd() | 1;
    // the fake car
    uint8_t* w = AR(A_WOBJ);
    for (int k = 0; k < 0x300; k += 4) { float f = range(-10.0f, 10.0f); memcpy(w + k, &f, 4); }
    *(uint8_t**)w = AR(A_VTAB);
    random_frame((float*)(w + 4), 500.0f);
    *(int32_t*)(w + 0x1c8) = (int32_t)(rnd() % 4);
    *(uint8_t**)(w + 0x28c) = AR(A_DETAIL);
    *(int32_t*)AR(A_DETAIL) = chance(90) ? (int32_t)(rnd() % 4) : (int32_t)rnd();
    void** vt = (void**)AR(A_VTAB);
    for (int i = 0; i < 16; i++) vt[i] = (void*)0x00000bad;
    vt[3] = (void*)&stub_wobj_visible;
    vt[8] = (void*)&stub_wobj_getz;
    // the camera, near the car
    float* cam = (float*)X_CAMERA;
    random_frame(cam, 1.0f);
    const float* wp = (const float*)(w + 0x28);
    const float cd = chance(50) ? range(0.0f, 40.0f) : range(0.0f, 160.0f);
    cam[9] = wp[0] + range(-cd, cd); cam[10] = wp[1] + range(-cd * 0.2f, cd * 0.2f); cam[11] = wp[2] + range(-cd, cd);
    if (chance(3)) { cam[9] = wp[0]; cam[10] = wp[1]; cam[11] = wp[2]; }
    // the shadow and the reflection
    uint8_t* sh = AR(A_SHADOW);
    *(uint32_t*)sh = 0x004dd258;
    *(uint8_t**)(sh + 4) = w;
    *(int32_t*)(sh + 8) = (int32_t)(rnd() % 4);
    *(float*)(sh + 0xc) = chance(50) ? range(0.0f, 12000.0f) : range(9000.0f, 11000.0f);
    *(uint8_t**)(sh + 0x10) = chance(80) ? AR(A_MODEL) : 0;
    *(uint8_t**)(sh + 0x14) = AR(A_HEAP) + 0x100 * (rnd() % 16);
    *(int32_t*)(sh + 0x18) = (int32_t)(rnd() % 50);
    uint8_t* rf = AR(A_REFL);
    *(uint32_t*)rf = 0x004dd2f8;
    *(uint8_t**)(rf + 4) = w;
    *(float*)(rf + 8) = chance(50) ? range(0.0f, 1200.0f) : range(850.0f, 950.0f);
    *(int32_t*)(rf + 0xc) = (int32_t)(rnd() % 50);
    *(int32_t*)(rf + 0x10) = 0; *(float*)(rf + 0x14) = 1.0f; *(int32_t*)(rf + 0x18) = 0;
    random_model();
    if (!g_setup_ok) return;
    // the splashes
    for (int i = 0; i < 32; i++) {
        uint8_t* s = splash(i);
        if (!s) continue;
        *(uint32_t*)(s + 4) = chance(90) ? 0 : k_volumes[rnd() % 4];
        rnd_p3(s + 0xc, -500.0f, 500.0f);
        for (int k = 0; k < 15; k++) rnd_p3(s + 0x18 + 12 * k, -6.0f, 6.0f);
        for (int k = 0; k < 15; k++) {
            uint8_t* vt2 = s + 0x18c + 32 * k;
            rnd_p3(vt2, -500.0f, 500.0f);
            *(uint32_t*)(vt2 + 0x10) = rnd();
        }
        const int kk = rnd() % 10;
        *(float*)(s + 0x36c) = kk < 2 ? -1000.0f : kk < 8 ? now - range(0.0f, 2.0f) : now + range(0.0f, 1.0f);
        *(float*)(s + 0x370) = chance(90) ? range(0.0f, 2.0f) : 0.0f;
        *(float*)(s + 0x374) = now - (chance(90) ? range(0.0f, 0.1f) : range(0.0f, 5.0f));
        *(float*)(s + 0x378) = chance(50) ? 0.7f : range(0.0f, 1.2f);
        s[0x37c] = (uint8_t)chance(50);
    }
    *(int32_t*)S_SPLASH_COUNT = (int32_t)(rnd() % 32);            // (the constructor writes g_tab.p[count]: no wild count)
    // the debris and its smoke
    uint8_t* d = debris();
    if (d) {
        for (int k = 0; k < 16; k++) rnd_p3(d + 4 + 12 * k, -10.0f, 10.0f);
        rnd_p3(d + 0xc4, -500.0f, 500.0f);
        *(int32_t*)(d + 0x114) = (int32_t)(rnd() % 16);
        const int kk = rnd() % 10;
        *(float*)(d + 0x118) = kk < 2 ? 1e6f : kk < 8 ? now - range(0.0f, 1.0f) : now + range(0.0f, 1.0f);
        for (int i = 0; i < 16; i++) {
            uint8_t* sm = smoke(i);
            if (!sm) continue;
            rnd_p3(sm + 0x28, -500.0f, 500.0f);
            sm[0x3c] = (uint8_t)chance(70);
            sm[0x3d] = (uint8_t)(chance(50) ? rnd() % 2 : rnd());
            rnd_p3(sm + 0x40, -0.05f, 0.05f);
            const float dur = chance(95) ? range(0.75f, 1.75f) : 0.0f;
            *(float*)(sm + 0x50) = dur;
            *(float*)(sm + 0x4c) = now + range(-0.3f, dur + 0.3f);
            *(float*)(sm + 0x54) = range(0.0f, 1.0f);
            if (chance(5)) *(int32_t*)(sm + 0x38) = -1;
        }
    }
    uint8_t* sp = sparks();
    if (sp) {
        *(int32_t*)(sp + 0x384) = (int32_t)(rnd() % 32);
        for (int k = 0; k < 32; k++) {
            uint8_t* p = sp + 4 + 0x1c * k;
            rnd_p3(p, -10.0f, 10.0f);
            rnd_p3(p + 0xc, -500.0f, 500.0f);
            const int kk = rnd() % 10;
            *(float*)(p + 0x18) = kk < 2 ? 1e6f : kk < 8 ? now - range(0.0f, 1.0f) : now + range(0.0f, 0.5f);
        }
    }
    // the skid marks and four clients
    uint8_t* so = AR(A_SKIDOBJ);
    const int32_t n = *(int32_t*)(so + 8);
    uint8_t* marks = *(uint8_t**)(so + 4);
    if (n > 0 && marks) {
        for (int k = 0; k < n; k++) {
            uint8_t* m = marks + 0x38 * k;
            float base[3]; rnd_p3(base, -300.0f, 300.0f);
            for (int e = 0; e < 4; e++) { float* p = (float*)(m + 12 * e); for (int c = 0; c < 3; c++) p[c] = base[c] + range(-1.0f, 1.0f); }
            *(int32_t*)(m + 0x30) = chance(60) ? 1 : (int32_t)(rnd() % 3);
            *(int32_t*)(m + 0x34) = (int32_t)(rnd() % 4);
        }
        *(int32_t*)(so + 0xc) = chance(80) ? (int32_t)(rnd() % n) : (int32_t)(rnd() % (2 * n));
    }
    for (int c = 0; c < 4; c++) {
        uint8_t* cl = AR(A_CLIENTS) + 0x40 * c;
        for (int k = 0; k < 0x40; k += 4) { float f = range(-10.0f, 10.0f); memcpy(cl + k, &f, 4); }
        uint8_t* m = n > 0 && marks && chance(60) ? marks + 0x38 * (rnd() % n) : 0;
        *(uint8_t**)(cl + 0x18) = m;
        *(uint8_t**)(cl + 0x1c) = so;
        const float* ref = m ? (const float*)(m + 0x18) : (const float*)(AR(A_WOBJ) + 0x28);
        for (int e = 0; e < 2; e++) { float* p = (float*)(cl + 12 * e); for (int k = 0; k < 3; k++) p[k] = ref[k] + range(-0.8f, 0.8f); }
        const int kk = rnd() % 10;
        *(float*)(cl + 0x20) = kk < 3 ? range(0.0f, 0.14f) : kk < 9 ? range(0.12f, 0.5f) : -range(0.0f, 1.0f);
        *(int32_t*)(cl + 0x24) = c;
        cl[0x28] = (uint8_t)(chance(50) ? rnd() % 2 : rnd());
    }
}

// ---- the functions under test --------------------------------------------------------------------------------------
enum Kind {
    K_FIND_IDX, K_FACES_UP, K_CREATE_SHADOW, K_EFFECTS_BEGIN, K_EFFECTS_END, K_SHADOW_CTOR, K_SHADOW_DTOR, K_SHADOW_VIS,
    K_CAM_IN_CAR, K_SHADOW_UPDATE, K_SHADOW_ORDER, K_DESTROY_SMOKE, K_SMOKE_ANIM, K_SMOKE_CTOR, K_CREATE_SMOKE, K_SMOKE_DTOR,
    K_SMOKE_VIS, K_SMOKE_ALPHA, K_REAL_RANDOM, K_PUFF, K_SMOKE_UPDATE, K_REFL_CTOR, K_REFL_DTOR, K_REFL_ORDER, K_REFL_UPDATE,
    K_REFL_VIS, K_FOOTENSOR, K_INTERPOLATE, K_SKID_CTOR, K_SKID_CLEAR, K_SKID_DTOR, K_SKID_ORDER, K_SKID_VIS, K_SKID_ALPHA,
    K_GET_SKID, K_RELEASE_SKID, K_SKID_ON, K_SKID_OFF, K_SKID_CL_CLEAR, K_SKIDMARK_CTOR, K_BUDDY_VIS, K_BUDDY_Z, K_GRAPH_Z,
    K_DEBRIS_BEGIN, K_PARTICLE_POS, K_DEBRIS_END, K_RANDOMREAL, K_DEBRIS_EVENT, K_SPARKS_EVENT, K_DEBRIS_VIS, K_DEBRIS_Z,
    K_DEBRIS_ALPHA, K_SPARKS_Z, K_SPARKS_VIS, K_SPARKS_ALPHA, K_PARTICLE_CTOR, K_RANDOM_REAL_I, K_GET_RANDOM_V, K_SPLASH,
    K_SPLASH_UPDATE, K_SPLASH_CTOR, K_SPLASH_DTOR, K_SPLASH_HANDLER, K_SPLASH_OBEGIN, K_SPLASH_OEND, K_SPLASH_BEGIN,
    K_SPLASH_END, K_SPLASH_VIS, K_SPLASH_ALPHA, K_SPLASH_Z, K_BAZ_CTOR,
    K_SEQ_SKID, K_SEQ_SPLASH, K_SEQ_DEBRIS, K_SEQ_VIEW, N_KINDS
};
static const char* const kind_names[N_KINDS] = {
    "find_idx_of", "faces_up", "create_shadow_model", "EffectsBegin", "EffectsEnd", "ShadowObject::ShadowObject",
    "ShadowObject::~ShadowObject", "ShadowObject::IsVisible", "camera_is_in_car", "ShadowObject::Update", "ShadowObject::Order",
    "destroy_smoke_model", "set_smoke_animation", "SmokeObject::SmokeObject", "create_smoke_model", "SmokeObject::~SmokeObject",
    "SmokeObject::IsVisible", "SmokeObject::IsAlpha", "real_random", "SmokeObject::Puff", "SmokeObject::Update",
    "ReflectionObject::ReflectionObject", "ReflectionObject::~ReflectionObject", "ReflectionObject::Order",
    "ReflectionObject::Update", "ReflectionObject::IsVisible", "footensor", "interpolate", "SkidObject::SkidObject",
    "SkidObject::Clear", "SkidObject::~SkidObject", "SkidObject::Order", "SkidObject::IsVisible", "SkidObject::IsAlpha",
    "SkidObject::GetSkid", "SkidObject::ReleaseSkid", "SkidClient::SkidOn", "SkidClient::SkidOff", "SkidClient::Clear",
    "SkidMark::SkidMark", "BuddyObject::IsVisible", "BuddyObject::GetZ", "GraphObject::GetZ", "DebrisBegin",
    "GetParticlePosition", "DebrisEnd", "RandomReal", "debris_event", "sparks_event", "DebrisObject::IsVisible",
    "DebrisObject::GetZ", "DebrisObject::IsAlpha", "SparksObject::GetZ", "SparksObject::IsVisible", "SparksObject::IsAlpha",
    "SparksObject::Particle::Particle", "random_real", "get_random_v", "SplashObject::Splash", "SplashObject::Update",
    "SplashObject::SplashObject", "SplashObject::~SplashObject", "SplashObject::Handler", "SplashObject::Begin",
    "SplashObject::End", "SplashBegin", "SplashEnd", "SplashObject::IsVisible", "SplashObject::IsAlpha", "SplashObject::GetZ",
    "SplashObject::baz::baz",
    "[frames] skids", "[frames] splashes", "[frames] crashes and smoke", "[frames] shadow and reflection"};
static int kind_weight(Kind k) {
    switch (k) {
    case K_SKID_ON: case K_SPLASH: case K_SPLASH_UPDATE: case K_DEBRIS_EVENT: case K_SPARKS_EVENT: case K_SMOKE_UPDATE:
    case K_PUFF: case K_SMOKE_ANIM: case K_SPLASH_HANDLER: case K_CREATE_SHADOW: return 60;
    case K_SEQ_SKID: case K_SEQ_SPLASH: case K_SEQ_DEBRIS: case K_SEQ_VIEW: return 30;
    case K_FACES_UP: case K_FIND_IDX: case K_GET_SKID: case K_SHADOW_CTOR: case K_SHADOW_VIS: case K_SHADOW_UPDATE:
    case K_REFL_UPDATE: case K_REFL_VIS: case K_SKID_CLEAR: case K_SKID_OFF: case K_FOOTENSOR: case K_INTERPOLATE:
    case K_PARTICLE_POS: case K_REAL_RANDOM: case K_RANDOMREAL: case K_RANDOM_REAL_I: case K_GET_RANDOM_V: case K_DEBRIS_VIS:
    case K_SKID_CTOR: return 20;
    default: return 5;
    }
}

struct Args {
    uint8_t* self; uint8_t* p; const P3* a; const P3* b; const P3* c; float f, g; int i, j; uint8_t flag;
    const char* name;
};
static Args g_args;

static uint8_t* any_smoke() { return debris() ? smoke(rnd() % 16) : 0; }
static uint8_t* any_mark() {
    uint8_t* so = AR(A_SKIDOBJ);
    const int32_t n = *(int32_t*)(so + 8);
    uint8_t* marks = *(uint8_t**)(so + 4);
    return n > 0 && marks ? marks + 0x38 * (rnd() % n) : AR(A_OBJ);
}
static void event_debris(uint8_t* e) {
    float* f = (float*)e;
    rnd_p3(f, -30.0f, 30.0f);
    rnd_p3(f + 3, -500.0f, 500.0f);
    rnd_p3(f + 6, -300.0f, 300.0f);
    f[7] = chance(30) ? range(-100.0f, 500.0f) : range(300.0f, 1000.0f);
    if (chance(5)) wild_at(&f[rnd() % 9]);
}
static void event_sparks(uint8_t* e) {
    float* f = (float*)e;
    rnd_p3(f, -30.0f, 30.0f);
    if (chance(5)) f[0] = f[1] = f[2] = 0.0f;
    rnd_p3(f + 3, -500.0f, 500.0f);
    if (chance(5)) wild_at(&f[rnd() % 6]);
}
static void event_splash(uint8_t* e) {
    float* f = (float*)e;
    f[0] = range(-8.0f, 8.0f); f[1] = chance(80) ? -range(0.0f, 12.0f) : range(-12.0f, 12.0f); f[2] = range(-8.0f, 8.0f);
    if (chance(10)) { f[0] = range(-0.3f, 0.3f); f[2] = range(-0.3f, 0.3f); }       // straight down: too small
    rnd_p3(f + 3, -500.0f, 500.0f);
    uint32_t vol = chance(50) ? k_volumes[rnd() % 4] : rnd();
    memcpy(e + 0x18, &vol, 4);
    if (chance(5)) wild_at(&f[rnd() % 6]);
}

static void random_args(Kind k) {
    memset(&g_args, 0, sizeof g_args);
    P3* pa = (P3*)ARG(G_P3A);
    P3* pb = (P3*)ARG(G_P3B);
    P3* pc = (P3*)ARG(G_P3C);
    g_args.a = pa; g_args.b = pb; g_args.c = pc;
    rnd_p3(pa, -10.0f, 10.0f); rnd_p3(pb, -10.0f, 10.0f); rnd_p3(pc, -10.0f, 10.0f);
    g_args.f = range(-2.0f, 2.0f); g_args.g = range(-2.0f, 2.0f);
    g_args.i = (int)(rnd() % 20) - 10; g_args.j = (int)(rnd() % 20) - 5;
    g_args.flag = (uint8_t)(chance(50) ? rnd() % 2 : rnd());
    static const char* const names[] = {"viper", "cobra", "", "a_rather_long_car_name"};
    strcpy((char*)ARG(G_NAME), names[rnd() % 4]);
    g_args.name = (const char*)ARG(G_NAME);
    switch (k) {
    case K_FIND_IDX: {
        int16_t* arr = (int16_t*)ARG(G_IDX);
        const int n = chance(90) ? (int)(rnd() % 40) : (int)(rnd() % 5) - 3;
        for (int i = 0; i < 40; i++) arr[i] = (int16_t)(chance(50) ? rnd() % 64 : rnd());
        g_args.p = (uint8_t*)arr;
        g_args.j = n;
        g_args.i = n > 0 && chance(80) ? arr[rnd() % n] : (int)(int16_t)rnd();
        if (chance(10)) g_args.i = (int)rnd();
        break;
    }
    case K_FACES_UP: {
        const int32_t* h = (const int32_t*)AR(A_MODEL);
        g_args.p = (uint8_t*)h[1];
        int16_t* tri = (int16_t*)ARG(G_TRI);
        if (chance(70)) memcpy(tri, (uint8_t*)h[5] + 8 * (rnd() % h[4]), 8);
        else for (int c = 0; c < 3; c++) tri[c] = (int16_t)(rnd() % h[0]);
        g_args.a = (const P3*)tri;
        break;
    }
    case K_SMOKE_ANIM: {
        uint8_t* s = any_smoke();
        g_args.self = s;
        g_args.p = s ? *(uint8_t**)(s + 0x34) : 0;
        g_args.i = s ? *(int32_t*)(s + 0x38) : 0;
        g_args.f = chance(80) ? range(-0.2f, 1.2f) : chance(50) ? (float)(rnd() % 10) / 7.9f : wildf();
        break;
    }
    case K_PUFF: case K_SMOKE_UPDATE: case K_SMOKE_VIS: case K_SMOKE_DTOR: case K_SMOKE_ALPHA:
        g_args.self = any_smoke();
        rnd_p3(pa, -500.0f, 500.0f);
        rnd_p3(pb, -30.0f, 30.0f);
        if (chance(5)) wild_at((float*)pb + rnd() % 3);
        break;
    case K_DESTROY_SMOKE: { uint8_t* s = any_smoke(); g_args.p = s ? *(uint8_t**)(s + 0x34) : 0; break; }
    case K_REAL_RANDOM: case K_RANDOMREAL:
        g_args.f = chance(70) ? range(-1.0f, 1.0f) : wildf();
        g_args.g = chance(70) ? range(-1.0f, 2.0f) : wildf();
        break;
    case K_RANDOM_REAL_I:
        g_args.i = chance(80) ? (int)(rnd() % 21) - 10 : (int)rnd();
        g_args.j = chance(80) ? (int)(rnd() % 21) - 10 : (int)rnd();
        break;
    case K_FOOTENSOR: case K_INTERPOLATE: case K_PARTICLE_POS:
        g_args.p = ARG(G_M3);
        rnd_p3(ARG(G_M3), -5.0f, 5.0f);
        if (chance(10)) wild_at((float*)pa + rnd() % 3);
        if (chance(10)) wild_at((float*)pb + rnd() % 3);
        if (chance(10)) g_args.f = wildf();
        if (k != K_FOOTENSOR && chance(20)) g_args.a = (const P3*)ARG(G_M3);   // the output as an input
        if (k == K_PARTICLE_POS && chance(20)) g_args.b = (const P3*)ARG(G_M3);
        break;
    case K_SKID_CTOR:
        g_args.i = chance(90) ? (int)(rnd() % 40) : (int)(rnd() % 4) - 2;
        if (chance(5)) g_script.alloc_fail = 1;
        break;
    case K_GET_SKID: g_args.i = (int)(rnd() % 4); break;
    case K_RELEASE_SKID: g_args.p = any_mark(); break;
    case K_SKID_ON: case K_SKID_OFF: case K_SKID_CL_CLEAR: {
        uint8_t* cl = AR(A_CLIENTS) + 0x40 * (rnd() % 4);
        g_args.self = cl;
        uint8_t* m = *(uint8_t**)(cl + 0x18);
        const float* ref = m && chance(70) ? (const float*)(m + 0x18) : (const float*)cl;
        const float step = chance(50) ? range(0.0f, 0.4f) : range(0.0f, 1.5f);
        for (int c = 0; c < 3; c++) ((float*)pa)[c] = ref[c] + range(-step, step);
        if (chance(3) && m) memcpy(pa, m, 12);                         // at the mark's start: no direction
        rnd_p3(pb, -1.0f, 1.0f);
        if (chance(5) && m) { float* d = (float*)pb; const float* s = (const float*)m; for (int c = 0; c < 3; c++) d[c] = ((float*)pa)[c] - s[c]; }
        if (chance(4)) wild_at((float*)pa + rnd() % 3);
        if (chance(4)) wild_at((float*)pb + rnd() % 3);
        if (chance(4)) wild_at(cl + 0x20);
        break;
    }
    case K_DEBRIS_EVENT: event_debris(ARG(G_EVENT)); g_args.p = ARG(G_EVENT); break;
    case K_SPARKS_EVENT: event_sparks(ARG(G_EVENT)); g_args.p = ARG(G_EVENT); break;
    case K_SPLASH_HANDLER: event_splash(ARG(G_EVENT)); g_args.p = ARG(G_EVENT);
        if (chance(30)) { uint8_t* s = splash(rnd() % 32); if (s) memcpy(ARG(G_EVENT) + 0x18, s + 4, 4); }   // one still on
        if (chance(10)) *(int32_t*)S_SPLASH_COUNT = (int32_t)rnd();         // (the handler masks it)
        break;
    case K_SPLASH: case K_SPLASH_UPDATE: case K_SPLASH_DTOR: case K_SPLASH_VIS: case K_SPLASH_ALPHA:
        g_args.self = splash(rnd() % 32);
        event_splash(ARG(G_EVENT));
        g_args.a = (const P3*)(ARG(G_EVENT) + 0xc);
        g_args.b = (const P3*)ARG(G_EVENT);
        g_args.p = *(uint8_t**)(ARG(G_EVENT) + 0x18);
        if (k == K_SPLASH_ALPHA && g_args.self && chance(30)) *(float*)(g_args.self + 0x378) = chance(50) ? 1.0f : wildf();
        break;
    case K_SPLASH_CTOR:
        if (chance(3)) *(int32_t*)S_SPLASH_COUNT = 32;                     // the 33rd: lands on the count
        break;
    case K_SPLASH_OBEGIN: case K_SPLASH_BEGIN: case K_EFFECTS_BEGIN: case K_DEBRIS_BEGIN:
        if (chance(10)) g_script.alloc_fail = rnd() & rnd();
        break;
    case K_SPLASH_OEND: case K_SPLASH_END: case K_EFFECTS_END: case K_DEBRIS_END:
        if (chance(20)) *(int32_t*)X_NUM_EVENTS = (int32_t)(rnd() % 4);   // some handlers already gone
        break;
    case K_SHADOW_DTOR: if (chance(20)) *(uint32_t*)(AR(A_SHADOW) + 0x10) = 0; break;
    default: break;
    }
}

static void wildify() {
    const int n = 1 + rnd() % 3;
    for (int i = 0; i < n; i++) {
        const int w = rnd() % 8;
        if (w == 0 && debris()) { uint8_t* sm = smoke(rnd() % 16); if (sm) { static const uint8_t offs[] = {0x28, 0x2c, 0x30, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x54}; wild_at(sm + offs[rnd() % 9]); } }   // (floats, not the model)
        else if (w == 1) { uint8_t* s = splash(rnd() % 32); if (s) wild_at(s + (chance(50) ? 0x36c + 4 * (rnd() % 4) : 0x18 + 4 * (rnd() % 45))); }
        else if (w == 2 && sparks()) wild_at(sparks() + 4 + 4 * (rnd() % (32 * 7)));
        else if (w == 3 && debris()) wild_at(debris() + (chance(50) ? 0x118 : 4 + 4 * (rnd() % 51)));
        else if (w == 4) wild_at((float*)X_CAMERA + 9 + rnd() % 3);
        else if (w == 5) wild_at(AR(A_WOBJ) + 0x28 + 4 * (rnd() % 3));
        else if (w == 6) { static const uint8_t offs[] = {0, 4, 8, 0xc, 0x10, 0x14, 0x20}; wild_at(AR(A_CLIENTS) + 0x40 * (rnd() % 4) + offs[rnd() % 7]); }   // (floats, not the pointers)
        else { uint8_t* so = AR(A_SKIDOBJ); const int32_t n2 = *(int32_t*)(so + 8); uint8_t* m = *(uint8_t**)(so + 4);
               if (n2 > 0 && m) wild_at(m + 0x38 * (rnd() % n2) + 4 * (rnd() % 12)); }
    }
}

// the originals
typedef int16_t(__cdecl* FindIdx_t2)(int, int16_t*, int);
typedef uint8_t(__cdecl* FacesUp_t2)(const uint8_t*, const int16_t*);
typedef void*(__cdecl* RetPtr_t)(const void*);
typedef void*(__cdecl* RetPtr0_t)();
typedef void*(__fastcall* Ctor2_t)(void*, int, const char*, void*);
typedef void*(__fastcall* Ctor1_t)(void*, int, int);
typedef void*(__fastcall* Ctor0_t)(void*, int);
typedef void(__fastcall* This_t)(void*, int);
typedef uint8_t(__fastcall* ThisB_t)(void*, int);
typedef int(__fastcall* ThisI_t)(void*, int);
typedef float(__fastcall* ThisF_t)(void*, int);
typedef uint8_t(__cdecl* B0_t)();
typedef void(__cdecl* V1_t)(void*);
typedef void(__cdecl* Anim_t)(int, void*, float, uint8_t);
typedef double(__cdecl* RR_t)(float, float);
typedef double(__cdecl* RRI_t)(int, int);
typedef void(__fastcall* PuffO_t)(void*, int, const P3*, const P3*, uint8_t);
typedef void(__cdecl* Foot_t)(void*, const P3*, const P3*);
typedef void(__cdecl* Interp_t)(P3*, const P3*, float);
typedef void(__cdecl* PPos_t)(P3*, const P3*, const P3*, float);
typedef uint8_t*(__fastcall* GetSkid_t2)(void*, int, int);
typedef void(__fastcall* Rel_t)(void*, int, void*);
typedef void(__fastcall* SkidOn_t)(void*, int, const P3*, const P3*);
typedef int(__cdecl* Event_t)(const void*);
typedef void(__fastcall* Splash_t2)(void*, int, const P3*, const P3*, void*);

static uint64_t u64f(float f) { return fbits(f); }
static uint64_t u64d(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

// one frame of the sequences, as each pass sees them (a pass calls the originals or the rewrites at the top)
static void seq_skid(bool rw, int frames) {
    for (int t = 0; t < frames; t++) {
        for (int c = 0; c < 4; c++) {
            uint8_t* cl = AR(A_CLIENTS) + 0x40 * c;
            P3* at = (P3*)(ARG(0x800) + 0x20 * c);
            P3* ax = (P3*)(ARG(0xa00) + 0x20 * c);
            const uint32_t r = (uint32_t)(t * 7 + c * 13) % 23;           // (the same in both passes)
            if (r < 17) rw ? SkidClient_SkidOn(cl, 0, at, ax) : ((SkidOn_t)0x00468ae0)(cl, 0, at, ax);
            else if (r < 20) rw ? SkidClient_SkidOff(cl, 0) : ((This_t)0x00468e20)(cl, 0);
            else if (r < 22) rw ? SkidClient_Clear(cl, 0) : ((This_t)0x00468e40)(cl, 0);
            float* p = (float*)at;
            const float* v = (const float*)(ARG(0xc00) + 0x10 * c);
            for (int k = 0; k < 3; k++) p[k] = (float)(p[k] + v[k]);        // the wheel moves on (same arithmetic)
        }
        if (t % 37 == 36) rw ? SkidObject_Clear(AR(A_SKIDOBJ), 0) : ((This_t)0x00468860)(AR(A_SKIDOBJ), 0);
    }
}
static void seq_splash(bool rw, int frames) {
    for (int t = 0; t < frames; t++) {
        *(int32_t*)X_TICK += 1 + (t % 3);
        if (t % 5 == 0) {
            const uint8_t* e = ARG(0x1000) + 0x20 * ((t / 5) % 16);
            log_word(rw ? (uint32_t)SplashObject_Handler(e) : (uint32_t)((Event_t)0x0046f780)(e));
        }
        for (int i = 0; i < 32; i++) {
            uint8_t* s = splash(i);
            if (!s) continue;
            rw ? SplashObject_Update(s, 0) : ((This_t)0x0046f3a0)(s, 0);
            log_word(rw ? SplashObject_IsVisible(s, 0) : ((ThisB_t)0x0046f860)(s, 0));
            log_word(rw ? SplashObject_IsAlpha(s, 0) : ((ThisB_t)0x0046f870)(s, 0));
        }
    }
}
static void seq_debris(bool rw, int frames) {
    for (int t = 0; t < frames; t++) {
        *(int32_t*)X_TICK += 1 + (t % 2);
        if (t % 3 == 0) {
            const uint8_t* e = ARG(0x1400) + 0x28 * ((t / 3) % 16);
            log_word(rw ? (uint32_t)debris_event(e) : (uint32_t)((Event_t)0x0046e770)(e));
        }
        if (t % 2 == 0) {
            const uint8_t* e = ARG(0x1800) + 0x20 * ((t / 2) % 16);
            log_word(rw ? (uint32_t)sparks_event(e) : (uint32_t)((Event_t)0x0046e930)(e));
        }
        if (!debris()) continue;
        for (int i = 0; i < 16; i++) {
            uint8_t* sm = smoke(i);
            if (!sm) continue;
            rw ? SmokeObject_Update(sm, 0) : ((This_t)0x004680e0)(sm, 0);
            log_word(rw ? SmokeObject_IsVisible(sm, 0) : ((ThisB_t)0x00467f80)(sm, 0));
        }
        log_word(rw ? DebrisObject_IsVisible(debris(), 0) : ((ThisB_t)0x0046ea80)(debris(), 0));
    }
}
static void seq_view(bool rw, int frames) {
    float* cam = (float*)X_CAMERA;
    for (int t = 0; t < frames; t++) {
        const float* v = (const float*)ARG(0x1c00);
        for (int k = 0; k < 3; k++) cam[9 + k] = (float)(cam[9 + k] + v[k]);
        if (t % 11 == 0) *(uint8_t*)X_MIRROR ^= 1;
        if (t % 13 == 0) *(int32_t*)X_CAMTYPE = (*(int32_t*)X_CAMTYPE + 3) % 12;
        rw ? ShadowObject_Update(AR(A_SHADOW), 0) : ((This_t)0x00467780)(AR(A_SHADOW), 0);
        log_word(rw ? ShadowObject_IsVisible(AR(A_SHADOW), 0) : ((ThisB_t)0x00467710)(AR(A_SHADOW), 0));
        rw ? ReflectionObject_Update(AR(A_REFL), 0) : ((This_t)0x00468320)(AR(A_REFL), 0);
        log_word(rw ? ReflectionObject_IsVisible(AR(A_REFL), 0) : ((ThisB_t)0x00468370)(AR(A_REFL), 0));
    }
}
static int g_frames;
static int g_max_faces;
static void seq_setup(Kind k) {
    g_frames = 20 + (int)(rnd() % 100);
    if (k == K_SEQ_SKID) {
        for (int c = 0; c < 4; c++) {
            uint8_t* cl = AR(A_CLIENTS) + 0x40 * c;
            memcpy(ARG(0x800) + 0x20 * c, cl, 12);
            float* ax = (float*)(ARG(0xa00) + 0x20 * c);
            ax[0] = range(-1.0f, 1.0f); ax[1] = range(-0.1f, 0.1f); ax[2] = range(-1.0f, 1.0f);
            float* v = (float*)(ARG(0xc00) + 0x10 * c);
            const float sp = chance(20) ? 0.0f : range(0.0f, 1.3f);
            v[0] = ax[2] * sp; v[1] = range(-0.01f, 0.01f); v[2] = -ax[0] * sp;
        }
    } else if (k == K_SEQ_SPLASH) {
        for (int e = 0; e < 16; e++) event_splash(ARG(0x1000) + 0x20 * e);
    } else if (k == K_SEQ_DEBRIS) {
        for (int e = 0; e < 16; e++) event_debris(ARG(0x1400) + 0x28 * e);
        for (int e = 0; e < 16; e++) event_sparks(ARG(0x1800) + 0x20 * e);
    } else if (k == K_SEQ_VIEW) {
        rnd_p3(ARG(0x1c00), -3.0f, 3.0f);
    }
}

static uint64_t call_kind(Kind k, bool rw) {
    Args& g = g_args;
    uint8_t* sh = AR(A_SHADOW);
    uint8_t* rf = AR(A_REFL);
    uint8_t* so = AR(A_SKIDOBJ);
    uint8_t* obj = AR(A_OBJ);
    switch (k) {
    case K_FIND_IDX: return (uint16_t)(rw ? find_idx_of(g.i, (int16_t*)g.p, g.j) : ((FindIdx_t2)0x00467310)(g.i, (int16_t*)g.p, g.j));
    case K_FACES_UP: return rw ? faces_up(g.p, (const int16_t*)g.a) : ((FacesUp_t2)0x00467350)(g.p, (const int16_t*)g.a);
    case K_CREATE_SHADOW: return (uint32_t)(rw ? (void*)create_shadow_model(g.name) : ((RetPtr_t)0x004670a0)(g.name));
    case K_EFFECTS_BEGIN: rw ? EffectsBegin() : ((Void_t2)0x00467460)(); return 0;
    case K_EFFECTS_END: rw ? EffectsEnd() : ((Void_t2)0x00467470)(); return 0;
    case K_SHADOW_CTOR: return (uint32_t)(rw ? (void*)ShadowObject_ctor(obj, 0, g.name, AR(A_WOBJ)) : ((Ctor2_t)0x00467480)(obj, 0, g.name, AR(A_WOBJ)));
    case K_SHADOW_DTOR: rw ? ShadowObject_dtor(sh, 0) : ((This_t)0x004676c0)(sh, 0); return 0;
    case K_SHADOW_VIS: return rw ? ShadowObject_IsVisible(sh, 0) : ((ThisB_t)0x00467710)(sh, 0);
    case K_CAM_IN_CAR: return rw ? camera_is_in_car() : ((B0_t)0x00467760)();
    case K_SHADOW_UPDATE: rw ? ShadowObject_Update(sh, 0) : ((This_t)0x00467780)(sh, 0); return 0;
    case K_SHADOW_ORDER: return (uint32_t)(rw ? ShadowObject_Order(sh, 0) : ((ThisI_t)0x00467b10)(sh, 0));
    case K_DESTROY_SMOKE: rw ? destroy_smoke_model((mrModelInfo*)g.p) : ((V1_t)0x00467b20)(g.p); return 0;
    case K_SMOKE_ANIM: if (!g.p) return 0;
        rw ? set_smoke_animation(g.i, (mrModelInfo*)g.p, g.f, g.flag) : ((Anim_t)0x00467b30)(g.i, g.p, g.f, g.flag); return 0;
    case K_SMOKE_CTOR: return (uint32_t)(rw ? (void*)SmokeObject_ctor(obj, 0) : ((Ctor0_t)0x00467c80)(obj, 0));
    case K_CREATE_SMOKE: return (uint32_t)(rw ? (void*)create_smoke_model() : ((RetPtr0_t)0x00467d70)());
    case K_SMOKE_DTOR: if (!g.self) return 0; rw ? SmokeObject_dtor(g.self, 0) : ((This_t)0x00467f40)(g.self, 0); return 0;
    case K_SMOKE_VIS: if (!g.self) return 0; return rw ? SmokeObject_IsVisible(g.self, 0) : ((ThisB_t)0x00467f80)(g.self, 0);
    case K_SMOKE_ALPHA: return rw ? SmokeObject_IsAlpha(g.self, 0) : ((ThisB_t)0x00467f90)(g.self, 0);
    case K_REAL_RANDOM: return u64d(rw ? real_random(g.f, g.g) : ((RR_t)0x004680b0)(g.f, g.g));
    case K_PUFF: if (!g.self) return 0;
        rw ? SmokeObject_Puff(g.self, 0, g.a, g.b, g.flag) : ((PuffO_t)0x00467fa0)(g.self, 0, g.a, g.b, g.flag); return 0;
    case K_SMOKE_UPDATE: if (!g.self) return 0; rw ? SmokeObject_Update(g.self, 0) : ((This_t)0x004680e0)(g.self, 0); return 0;
    case K_REFL_CTOR: return (uint32_t)(rw ? (void*)ReflectionObject_ctor(obj, 0, g.name, AR(A_WOBJ)) : ((Ctor2_t)0x00468290)(obj, 0, g.name, AR(A_WOBJ)));
    case K_REFL_DTOR: rw ? ReflectionObject_dtor(rf, 0) : ((This_t)0x004682f0)(rf, 0); return 0;
    case K_REFL_ORDER: return (uint32_t)(rw ? ReflectionObject_Order(rf, 0) : ((ThisI_t)0x00468310)(rf, 0));
    case K_REFL_UPDATE: rw ? ReflectionObject_Update(rf, 0) : ((This_t)0x00468320)(rf, 0); return 0;
    case K_REFL_VIS: return rw ? ReflectionObject_IsVisible(rf, 0) : ((ThisB_t)0x00468370)(rf, 0);
    case K_FOOTENSOR: rw ? footensor((M3*)g.p, g.a, g.b) : ((Foot_t)0x004683b0)(g.p, g.a, g.b); return 0;
    case K_INTERPOLATE: rw ? interpolate((P3*)g.p, g.a, g.f) : ((Interp_t)0x00468780)((P3*)g.p, g.a, g.f); return 0;
    case K_SKID_CTOR: return (uint32_t)(rw ? (void*)SkidObject_ctor(obj, 0, g.i) : ((Ctor1_t)0x004687d0)(obj, 0, g.i));
    case K_SKID_CLEAR: rw ? SkidObject_Clear(so, 0) : ((This_t)0x00468860)(so, 0); return 0;
    case K_SKID_DTOR: rw ? SkidObject_dtor(so, 0) : ((This_t)0x00468890)(so, 0); return 0;
    case K_SKID_ORDER: return (uint32_t)(rw ? SkidObject_Order(so, 0) : ((ThisI_t)0x004688c0)(so, 0));
    case K_SKID_VIS: return rw ? SkidObject_IsVisible(so, 0) : ((ThisB_t)0x00468a60)(so, 0);
    case K_SKID_ALPHA: return rw ? SkidObject_IsAlpha(so, 0) : ((ThisB_t)0x00468f50)(so, 0);
    case K_GET_SKID: return (uint32_t)(rw ? SkidObject_GetSkid(so, 0, g.i) : ((GetSkid_t2)0x00468a70)(so, 0, g.i));
    case K_RELEASE_SKID: rw ? SkidObject_ReleaseSkid(so, 0, g.p) : ((Rel_t)0x00468ad0)(so, 0, g.p); return 0;
    case K_SKID_ON: rw ? SkidClient_SkidOn(g.self, 0, g.a, g.b) : ((SkidOn_t)0x00468ae0)(g.self, 0, g.a, g.b); return 0;
    case K_SKID_OFF: rw ? SkidClient_SkidOff(g.self, 0) : ((This_t)0x00468e20)(g.self, 0); return 0;
    case K_SKID_CL_CLEAR: rw ? SkidClient_Clear(g.self, 0) : ((This_t)0x00468e40)(g.self, 0); return 0;
    case K_SKIDMARK_CTOR: return (uint32_t)(rw ? (void*)SkidMark_ctor(obj, 0) : ((Ctor0_t)0x00468f90)(obj, 0));
    case K_BUDDY_VIS: return rw ? BuddyObject_IsVisible(sh, 0) : ((ThisB_t)0x00468e70)(sh, 0);
    case K_BUDDY_Z: return u64f(rw ? (float)BuddyObject_GetZ(sh, 0) : ((ThisF_t)0x00468e80)(sh, 0));
    case K_GRAPH_Z: return u64f(rw ? GraphObject_GetZ(sh, 0) : ((ThisF_t)0x00468f30)(sh, 0));
    case K_DEBRIS_BEGIN: rw ? DebrisBegin() : ((Void_t2)0x0046e570)(); return 0;
    case K_PARTICLE_POS: rw ? GetParticlePosition((P3*)g.p, g.a, g.b, g.f) : ((PPos_t)0x0046e700)((P3*)g.p, g.a, g.b, g.f); return 0;
    case K_DEBRIS_END: rw ? DebrisEnd() : ((Void_t2)0x0046e750)(); return 0;
    case K_RANDOMREAL: return u64d(rw ? RandomReal(g.f, g.g) : ((RR_t)0x0046e900)(g.f, g.g));
    case K_DEBRIS_EVENT: if (!debris()) return 0; return (uint32_t)(rw ? debris_event(g.p) : ((Event_t)0x0046e770)(g.p));
    case K_SPARKS_EVENT: if (!sparks()) return 0; return (uint32_t)(rw ? sparks_event(g.p) : ((Event_t)0x0046e930)(g.p));
    case K_DEBRIS_VIS: if (!debris()) return 0; return rw ? DebrisObject_IsVisible(debris(), 0) : ((ThisB_t)0x0046ea80)(debris(), 0);
    case K_DEBRIS_Z: return u64f(rw ? Effect_GetZ(debris(), 0) : ((ThisF_t)0x0046ead0)(debris(), 0));
    case K_DEBRIS_ALPHA: return rw ? Effect_True(debris(), 0) : ((ThisB_t)0x0046eae0)(debris(), 0);
    case K_SPARKS_Z: return u64f(rw ? SparksObject_GetZ(sparks(), 0) : ((ThisF_t)0x0046eb60)(sparks(), 0));
    case K_SPARKS_VIS: return rw ? SparksObject_IsVisible(sparks(), 0) : ((ThisB_t)0x0046eb50)(sparks(), 0);
    case K_SPARKS_ALPHA: return rw ? SparksObject_IsAlpha(sparks(), 0) : ((ThisB_t)0x0046eb70)(sparks(), 0);
    case K_PARTICLE_CTOR: return (uint32_t)(rw ? (void*)Particle_ctor(obj, 0) : ((Ctor0_t)0x0046eda0)(obj, 0));
    case K_RANDOM_REAL_I: return u64d(rw ? random_real(g.i, g.j) : ((RRI_t)0x0046f360)(g.i, g.j));
    case K_GET_RANDOM_V: rw ? get_random_v((P3*)ARG(G_M3)) : ((V1_t)0x0046f320)(ARG(G_M3)); return 0;
    case K_SPLASH: if (!g.self) return 0;
        rw ? SplashObject_Splash(g.self, 0, g.a, g.b, g.p) : ((Splash_t2)0x0046efd0)(g.self, 0, g.a, g.b, g.p); return 0;
    case K_SPLASH_UPDATE: if (!g.self) return 0; rw ? SplashObject_Update(g.self, 0) : ((This_t)0x0046f3a0)(g.self, 0); return 0;
    case K_SPLASH_CTOR: return (uint32_t)(rw ? (void*)SplashObject_ctor(obj, 0) : ((Ctor0_t)0x0046f590)(obj, 0));
    case K_SPLASH_DTOR: if (!g.self) return 0; rw ? SplashObject_dtor(g.self, 0) : ((This_t)0x0046f730)(g.self, 0); return 0;
    case K_SPLASH_HANDLER: return (uint32_t)(rw ? SplashObject_Handler(g.p) : ((Event_t)0x0046f780)(g.p));
    case K_SPLASH_OBEGIN: rw ? SplashObject_Begin() : ((Void_t2)0x0046f7e0)(); return 0;
    case K_SPLASH_OEND: rw ? SplashObject_End() : ((Void_t2)0x0046f830)(); return 0;
    case K_SPLASH_BEGIN: rw ? SplashBegin() : ((Void_t2)0x0046f840)(); return 0;
    case K_SPLASH_END: rw ? SplashEnd() : ((Void_t2)0x0046f850)(); return 0;
    case K_SPLASH_VIS: if (!g.self) return 0; return rw ? SplashObject_IsVisible(g.self, 0) : ((ThisB_t)0x0046f860)(g.self, 0);
    case K_SPLASH_ALPHA: if (!g.self) return 0; return rw ? SplashObject_IsAlpha(g.self, 0) : ((ThisB_t)0x0046f870)(g.self, 0);
    case K_SPLASH_Z: return u64f(rw ? SplashObject_GetZ(g.self, 0) : ((ThisF_t)0x0046f880)(g.self, 0));
    case K_BAZ_CTOR: return (uint32_t)(rw ? (void*)baz_ctor(obj, 0) : ((Ctor0_t)0x0046f8b0)(obj, 0));
    case K_SEQ_SKID: seq_skid(rw, g_frames); return 0;
    case K_SEQ_SPLASH: seq_splash(rw, g_frames); return 0;
    case K_SEQ_DEBRIS: seq_debris(rw, g_frames); return 0;
    case K_SEQ_VIEW: seq_view(rw, g_frames); return 0;
    default: return 0;
    }
}
static bool returns_byte(Kind k) {
    switch (k) {
    case K_FACES_UP: case K_SHADOW_VIS: case K_CAM_IN_CAR: case K_SMOKE_VIS: case K_SMOKE_ALPHA: case K_REFL_VIS: case K_SKID_VIS:
    case K_SKID_ALPHA: case K_BUDDY_VIS: case K_DEBRIS_VIS: case K_DEBRIS_ALPHA: case K_SPARKS_VIS: case K_SPARKS_ALPHA:
    case K_SPLASH_VIS: case K_SPLASH_ALPHA: return true;
    default: return false;
    }
}

static void footprint_of(Kind k, Footprint& fp) {
    fp.n = 0; fp.pure = false; fp.replay_only = 0;
    Args& g = g_args;
    uint8_t* sh = AR(A_SHADOW);
    uint8_t* rf = AR(A_REFL);
    uint8_t* so = AR(A_SKIDOBJ);
    uint8_t* obj = AR(A_OBJ);
    switch (k) {
    case K_FIND_IDX: fpof_find_idx_of(fp, g.i, (int16_t*)g.p, g.j); break;
    case K_FACES_UP: fpof_faces_up(fp, g.p, (const int16_t*)g.a); break;
    case K_CREATE_SHADOW: fpof_create_shadow_model(fp, g.name); break;
    case K_EFFECTS_BEGIN: fpof_EffectsBegin(fp); break;
    case K_EFFECTS_END: fpof_EffectsEnd(fp); break;
    case K_SHADOW_CTOR: fpof_ShadowObject_ctor(fp, obj, 0, g.name, AR(A_WOBJ)); break;
    case K_SHADOW_DTOR: fpof_ShadowObject_dtor(fp, sh, 0); break;
    case K_SHADOW_VIS: fpof_ShadowObject_IsVisible(fp, sh, 0); break;
    case K_CAM_IN_CAR: fpof_camera_is_in_car(fp); break;
    case K_SHADOW_UPDATE: fpof_ShadowObject_Update(fp, sh, 0); break;
    case K_SHADOW_ORDER: fpof_ShadowObject_Order(fp, sh, 0); break;
    case K_DESTROY_SMOKE: fpof_destroy_smoke_model(fp, (mrModelInfo*)g.p); break;
    case K_SMOKE_ANIM: fpof_set_smoke_animation(fp, g.i, (mrModelInfo*)g.p, g.f, g.flag); break;
    case K_SMOKE_CTOR: fpof_SmokeObject_ctor(fp, obj, 0); break;
    case K_CREATE_SMOKE: fpof_create_smoke_model(fp); break;
    case K_SMOKE_DTOR: fpof_SmokeObject_dtor(fp, g.self, 0); break;
    case K_SMOKE_VIS: fpof_SmokeObject_IsVisible(fp, g.self, 0); break;
    case K_SMOKE_ALPHA: fpof_SmokeObject_IsAlpha(fp, g.self, 0); break;
    case K_REAL_RANDOM: fpof_real_random(fp, g.f, g.g); break;
    case K_PUFF: if (g.self) fpof_SmokeObject_Puff(fp, g.self, 0, g.a, g.b, g.flag); break;
    case K_SMOKE_UPDATE: if (g.self) fpof_SmokeObject_Update(fp, g.self, 0); break;
    case K_REFL_CTOR: fpof_ReflectionObject_ctor(fp, obj, 0, g.name, AR(A_WOBJ)); break;
    case K_REFL_DTOR: fpof_ReflectionObject_dtor(fp, rf, 0); break;
    case K_REFL_ORDER: fpof_ReflectionObject_Order(fp, rf, 0); break;
    case K_REFL_UPDATE: fpof_ReflectionObject_Update(fp, rf, 0); break;
    case K_REFL_VIS: fpof_ReflectionObject_IsVisible(fp, rf, 0); break;
    case K_FOOTENSOR: fpof_footensor(fp, (M3*)g.p, g.a, g.b); fp.add(g.p, 36, "the matrix (pure)"); break;
    case K_INTERPOLATE: fpof_interpolate(fp, (P3*)g.p, g.a, g.f); fp.add(g.p, 12, "p (pure)"); break;
    case K_PARTICLE_POS: fpof_GetParticlePosition(fp, (P3*)g.p, g.a, g.b, g.f); fp.add(g.p, 12, "out (pure)"); break;
    case K_SKID_CTOR: fpof_SkidObject_ctor(fp, obj, 0, g.i); break;
    case K_SKID_CLEAR: fpof_SkidObject_Clear(fp, so, 0); break;
    case K_SKID_DTOR: fpof_SkidObject_dtor(fp, so, 0); break;
    case K_SKID_ORDER: fpof_SkidObject_Order(fp, so, 0); break;
    case K_SKID_VIS: fpof_SkidObject_IsVisible(fp, so, 0); break;
    case K_SKID_ALPHA: fpof_SkidObject_IsAlpha(fp, so, 0); break;
    case K_GET_SKID: fpof_SkidObject_GetSkid(fp, so, 0, g.i); break;
    case K_RELEASE_SKID: fpof_SkidObject_ReleaseSkid(fp, so, 0, g.p); break;
    case K_SKID_ON: fpof_SkidClient_SkidOn(fp, g.self, 0, g.a, g.b); break;
    case K_SKID_OFF: fpof_SkidClient_SkidOff(fp, g.self, 0); break;
    case K_SKID_CL_CLEAR: fpof_SkidClient_Clear(fp, g.self, 0); break;
    case K_SKIDMARK_CTOR: fpof_SkidMark_ctor(fp, obj, 0); break;
    case K_BUDDY_VIS: fpof_BuddyObject_IsVisible(fp, sh, 0); break;
    case K_BUDDY_Z: fpof_BuddyObject_GetZ(fp, sh, 0); break;
    case K_GRAPH_Z: fpof_GraphObject_GetZ(fp, sh, 0); break;
    case K_DEBRIS_BEGIN: fpof_DebrisBegin(fp); break;
    case K_DEBRIS_END: fpof_DebrisEnd(fp); break;
    case K_RANDOMREAL: fpof_RandomReal(fp, g.f, g.g); break;
    case K_DEBRIS_EVENT: fpof_debris_event(fp, g.p); break;
    case K_SPARKS_EVENT: fpof_sparks_event(fp, g.p); break;
    case K_DEBRIS_VIS: if (debris()) fpof_DebrisObject_IsVisible(fp, debris(), 0); break;
    case K_DEBRIS_Z: fpof_Effect_GetZ(fp, debris(), 0); break;
    case K_DEBRIS_ALPHA: fpof_Effect_True(fp, debris(), 0); break;
    case K_SPARKS_Z: fpof_SparksObject_GetZ(fp, sparks(), 0); break;
    case K_SPARKS_VIS: fpof_SparksObject_IsVisible(fp, sparks(), 0); break;
    case K_SPARKS_ALPHA: fpof_SparksObject_IsAlpha(fp, sparks(), 0); break;
    case K_PARTICLE_CTOR: fpof_Particle_ctor(fp, obj, 0); break;
    case K_RANDOM_REAL_I: fpof_random_real(fp, g.i, g.j); break;
    case K_GET_RANDOM_V: fpof_get_random_v(fp, (P3*)ARG(G_M3)); break;
    case K_SPLASH: if (g.self) fpof_SplashObject_Splash(fp, g.self, 0, g.a, g.b, g.p); break;
    case K_SPLASH_UPDATE: if (g.self) fpof_SplashObject_Update(fp, g.self, 0); break;
    case K_SPLASH_CTOR: fpof_SplashObject_ctor(fp, obj, 0); break;
    case K_SPLASH_DTOR: fpof_SplashObject_dtor(fp, g.self, 0); break;
    case K_SPLASH_HANDLER: fpof_SplashObject_Handler(fp, g.p); break;
    case K_SPLASH_OBEGIN: fpof_SplashObject_Begin(fp); break;
    case K_SPLASH_OEND: fpof_SplashObject_End(fp); break;
    case K_SPLASH_BEGIN: fpof_SplashBegin(fp); break;
    case K_SPLASH_END: fpof_SplashEnd(fp); break;
    case K_SPLASH_VIS: if (g.self) fpof_SplashObject_IsVisible(fp, g.self, 0); break;
    case K_SPLASH_ALPHA: if (g.self) fpof_SplashObject_IsAlpha(fp, g.self, 0); break;
    case K_SPLASH_Z: fpof_SplashObject_GetZ(fp, g.self, 0); break;
    case K_BAZ_CTOR: fpof_baz_ctor(fp, obj, 0); break;
    default: fp.replay_only = "a sequence of calls"; break;
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
static bool g_debug_cw = getenv("VP_CW") != 0;
static bool g_unmask;               // overflow and divide-by-zero unmasked (docs/PORTING.md 10a): a dropped store shows
static int run_guarded(Kind k, bool rw, uint64_t* ret) {
    g_log.n = 0;
    g_heap = g_heap_mark;
    g_alloc_calls = 0;
    g_rand = g_script.rand_seed;
    const int tex0 = g_script.tex;
    const int model0 = g_model_next;
    unsigned cw;
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);                                        // (an earlier fault leaves its registers on the x87 stack)
#else
    __asm fninit                                        // (an earlier fault leaves its registers on the x87 stack)
#endif
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    if (g_unmask) _controlfp_s(&cw, _MCW_EM & ~(_EM_OVERFLOW | _EM_ZERODIVIDE), _MCW_EM);
#if defined(__GNUC__) && !defined(__clang__)
    if (g_debug_cw && k == K_FACES_UP) { uint16_t x; uint16_t sw; __asm__ volatile("fnstcw %0\n\tfnstsw %1" : "=m"(x), "=m"(sw));
                                                                             printf("  pass %d: cw %04x sw %04x\n", rw, x, sw); }
#else
    if (g_debug_cw && k == K_FACES_UP) { uint16_t x; uint16_t sw; __asm { fnstcw x
                                                                             fnstsw sw } printf("  pass %d: cw %04x sw %04x\n", rw, x, sw); }
#endif
    int fault = 0;
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] {
        *ret = call_kind(k, rw);
        __asm__ volatile("fwait" ::: VP_X87_CLOBBERS, "memory");                                     // (an exception still pending from the call's last store)
    }, fault_filter)) { fault = 1; }
#else
    __try {
        *ret = call_kind(k, rw);
        __asm fwait                                     // (an exception still pending from the call's last store)
    } __except (fault_filter(GetExceptionInformation())) { fault = 1; }
#endif
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);                                        // (a fault leaves the x87 stack as it was)
#else
    __asm fninit                                        // (a fault leaves the x87 stack as it was)
#endif
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    g_script.tex = tex0;
    g_model_next = model0;
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
    int iterations = argc > 1 ? atoi(argv[1]) : 100000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_wld_effects.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);         // _amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);         // _NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);         // _FF_MSGBANNER
    patch_jmp(0x004d5c10, (void*)&stub_crt_fatal);         // _fptrap
    patch_jmp(0x004d6850, (void*)&stub_crt_msgbox);        // __crtMessageBoxA
    ((void(__cdecl*)())0x004ce150)();                      // the CRT's floating point, as its start-up leaves it
    patch_jmp(0x004140e0, (void*)&stub_MemAlloc);
    patch_jmp(0x00414300, (void*)&stub_MemFree);
    patch_jmp(0x00414390, (void*)&stub_delete);
    patch_jmp(0x004112b0, (void*)&stub_panic);
    patch_jmp(0x004cf0a0, (void*)&stub_sprintf);
    patch_jmp(0x0041b6e0, (void*)&stub_random);
    patch_jmp(0x004562f0, (void*)&stub_info_get);
    patch_jmp(0x00456460, (void*)&stub_info_forget);
    patch_jmp(0x00455b40, (void*)&stub_model_create);
    patch_jmp(0x00455c80, (void*)&stub_model_build);
    patch_jmp(0x00455bc0, (void*)&stub_model_destroy);
    patch_jmp(0x004558c0, (void*)&stub_model_load);
    patch_jmp(0x00455950, (void*)&stub_model_unload);
    patch_jmp(0x0044e240, (void*)&stub_gx_get_texture);
    patch_jmp(0x0044e280, (void*)&stub_gx_forget);
    patch_jmp(0x0045a0c0, (void*)&stub_texture_get);
    patch_jmp(0x0045a110, (void*)&stub_texture_forget);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_arena_snap = (uint8_t*)malloc(ARENA_BYTES);
    g_arena_after = (uint8_t*)malloc(ARENA_BYTES);
    g_data_snap = (uint8_t*)malloc(DATA_BYTES);
    g_data_after = (uint8_t*)malloc(DATA_BYTES);

    static Footprint fp;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    int unmasked_runs = 0;
    int differ = 0, faults = 0, fault_both = 0, fp_bad = 0, replay_only = 0, wild_runs = 0, wild_differ = 0, setup_bad = 0;
    int per[N_KINDS] = {0}, per_bad[N_KINDS] = {0};
    // coverage, from the starting state and the original's results
    int c_new_mark = 0, c_release = 0, c_skid_degenerate = 0, c_skid_first = 0, c_skid_extend = 0, c_skid_full = 0;
    int c_burst = 0, c_puff_only = 0, c_spark = 0, c_spark_skip = 0, c_splash_fresh = 0, c_splash_dup = 0, c_splash_small = 0,
        c_splash_capped = 0, c_splash_vis = 0, c_splash_hidden = 0, c_smoke_paused = 0, c_smoke_expired = 0, c_smoke_hi = 0,
        c_smoke_lo = 0, c_anim_out = 0, c_anim_in = 0, c_cap256 = 0, c_idx_panic = 0, c_shadow_vis = 0, c_shadow_hid = 0,
        c_nomodel = 0, c_getskid_none = 0, c_alloc_null = 0, c_count32 = 0;
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weight((Kind)i);
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), kk = 0;
        while (pick >= kind_weight((Kind)kk)) pick -= kind_weight((Kind)kk++);
        const Kind kind = (Kind)kk;
        setup_world();
        if (!g_setup_ok) { setup_bad++; continue; }
        random_world();
        random_args(kind);
        if (kind >= K_SEQ_SKID) seq_setup(kind);
        const bool wild = chance(10);
        if (wild) { wildify(); wild_runs++; }
        g_pc = chance(50) ? _PC_53 : _PC_24;
        g_unmask = chance(30);
        if (g_unmask) unmasked_runs++;
        if (trace) printf("world %d: %s\n", it, kind_names[kind]);
        footprint_of(kind, fp);
        if (fp.replay_only) replay_only++;
        if (kind == K_SPLASH_CTOR && *(int32_t*)S_SPLASH_COUNT == 32) c_count32++;
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
        if (fo || fn) {
            faults++;
            if (fo && fn) { fault_both++; if (fault_both <= 8) printf("  (world %d, %s: both faulted, %08x at %08x)\n", it, kind_names[kind], fo_code, fo_eip); }
            if (fo != fn) {
                printf("  world %d (%s): %s / %s; fault %08x at %08x (original's %08x at %08x), address %08x\n", it, kind_names[kind],
                       fo ? "original faulted" : "original ran", fn ? "rewrite faulted" : "rewrite ran", g_fault_code, g_fault_eip,
                       fo_code, fo_eip, g_fault_addr);
                differ++; per_bad[kind]++;
                if (kind == K_FACES_UP) {
                    const int16_t* t = (const int16_t*)g_args.a;
                    for (int c = 0; c < 3; c++) {
                        const float* v = (const float*)(g_args.p + 32 * t[c]);
                        printf("    v%d (%d): %08x %08x %08x\n", c, t[c], fbits(v[0]), fbits(v[1]), fbits(v[2]));
                    }
                    printf("    pc %s, unmasked %d, returned %llx\n", g_pc == _PC_24 ? "24" : "53", g_unmask, (unsigned long long)rn);
                }
            }
            continue;
        }
        if (returns_byte(kind)) { ro &= 0xff; rn &= 0xff; }
        bool same = true;
        char buf[64];
        for (uint32_t i = 0; i < ARENA_BYTES; i += 4)
            if (memcmp(g_arena_after + i, g_arena + i, 4)) {
                uint32_t x, y; memcpy(&x, g_arena_after + i, 4); memcpy(&y, g_arena + i, 4);
                if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
                printf("    %s original %08x (%.9g) rewrite %08x (%.9g)\n", where((uint32_t)(uintptr_t)(g_arena + i), buf), x, bitsf(x), y, bitsf(y));
                same = false;
                break;
            }
        for (uint32_t i = 0; i < DATA_BYTES; i += 4)
            if (memcmp(g_data_after + i, DATA + i, 4)) {
                uint32_t x, y; memcpy(&x, g_data_after + i, 4); memcpy(&y, DATA + i, 4);
                if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
                printf("    %s original %08x (%.9g) rewrite %08x (%.9g)\n", where((uint32_t)(uintptr_t)(DATA + i), buf), x, bitsf(x), y, bitsf(y));
                same = false;
                break;
            }
        if (ro != rn) {
            if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
            printf("    return: original %016llx rewrite %016llx\n", (unsigned long long)ro, (unsigned long long)rn);
            same = false;
        }
        if (g_log.n != g_log_orig.n || memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX))) {
            if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
            printf("    the stub logs differ: %u / %u words\n", g_log_orig.n, g_log.n);
            uint32_t m = g_log.n < g_log_orig.n ? g_log.n : g_log_orig.n;
            if (m > LOG_MAX) m = LOG_MAX;
            for (uint32_t i = 0; i < m; i++) if (g_log.w[i] != g_log_orig.w[i]) { printf("    first at %u: %08x / %08x\n", i, g_log_orig.w[i], g_log.w[i]); break; }
            same = false;
        }
        if (!same) {
            differ++; per_bad[kind]++;
            if (wild) wild_differ++;
            printf("    (pc %s, wild %d, exceptions %s)\n", g_pc == _PC_24 ? "24" : "53", wild, g_unmask ? "unmasked" : "masked");
            if (differ >= 30) { printf("stopping after 30 differing worlds\n"); break; }
        }
        // the original's writes against the footprint
        if (!fp.replay_only) {
            bool bad = false;
            for (uint32_t i = 0; i < ARENA_BYTES && !bad; i++)
                if (g_arena_after[i] != g_arena_snap[i] && !in_footprint(fp, g_arena + i)) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, kind_names[kind], where((uint32_t)(uintptr_t)(g_arena + i), buf));
                    bad = true;
                }
            for (uint32_t i = 0; i < DATA_BYTES && !bad; i++)
                if (g_data_after[i] != g_data_snap[i] && !in_footprint(fp, DATA + i)) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, kind_names[kind], where(0x4e1000 + i, buf));
                    bad = true;
                }
            fp_bad += bad;
        }
        // coverage
        auto logged = [&](uint32_t tag) { for (uint32_t i = 0; i < g_log_orig.n && i < LOG_MAX; i++) if (g_log_orig.w[i] == tag) return true; return false; };
        auto changed = [&](const void* p, uint32_t n) { const uint32_t o = (uint32_t)((const uint8_t*)p - g_arena); return memcmp(g_arena_snap + o, g_arena_after + o, n) != 0; };
        if (kind == K_SKID_ON) {
            const uint8_t* cl0 = g_arena_snap + (g_args.self - g_arena);
            const uint8_t* cl1 = g_arena_after + (g_args.self - g_arena);
            const uint32_t m0 = *(const uint32_t*)(cl0 + 0x18), m1 = *(const uint32_t*)(cl1 + 0x18);
            if (!m0 && m1) c_new_mark++;
            else if (m0 && m0 != m1) c_release++;
            if (!m1) c_skid_full++;
            if (m0 && m0 == m1) {
                if (!changed((void*)(m1 + 0x18), 24)) c_skid_degenerate++;
                else if (cl0[0x28]) c_skid_first++;
                else c_skid_extend++;
            }
        }
        if (kind == K_GET_SKID && ro == 0) c_getskid_none++;
        if (kind == K_DEBRIS_EVENT && debris()) (changed(debris() + 0x118, 4) ? c_burst : c_puff_only)++;
        if (kind == K_SPARKS_EVENT && sparks()) (changed(sparks() + 0x384, 4) ? c_spark : c_spark_skip)++;
        if (kind == K_SPLASH_HANDLER) (logged('RAND') ? c_splash_fresh : c_splash_dup)++;
        if (kind == K_SPLASH && g_args.self) {
            if (!changed(g_args.self + 0x36c, 4)) c_splash_small++;
            const float* v = (const float*)ARG(G_EVENT);
            if ((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2] > 25.0) c_splash_capped++;
        }
        if (kind == K_SPLASH_UPDATE && g_args.self) (g_arena_after[(g_args.self - g_arena) + 0x37c] ? c_splash_vis : c_splash_hidden)++;
        if (kind == K_SMOKE_UPDATE && g_args.self) {
            const uint8_t* s0 = g_arena_snap + (g_args.self - g_arena);
            const uint8_t* s1 = g_arena_after + (g_args.self - g_arena);
            if (s0[0x3c] && *(uint8_t*)(g_data_snap + (X_PAUSED - 0x4e1000))) c_smoke_paused++;
            if (s0[0x3c] && !s1[0x3c]) c_smoke_expired++;
            if (s1[0x3c]) { float l = *(const float*)(s1 + 0x4c) - now_time(); (l >= 1.0f ? c_smoke_hi : c_smoke_lo)++; }
        }
        if (kind == K_SMOKE_ANIM) (logged('MBLD') ? c_anim_in : c_anim_out)++;
        if (kind == K_CREATE_SHADOW) {
            if (ro) { const int32_t* h = (const int32_t*)(uintptr_t)ro; if (h[4] == 256) c_cap256++; if (h[4] > g_max_faces) g_max_faces = h[4]; }
            else c_nomodel++;
        }
        if (kind == K_FIND_IDX && logged('PANC')) c_idx_panic++;
        if (kind == K_SHADOW_VIS) (ro ? c_shadow_vis : c_shadow_hid)++;
        if (g_script.alloc_fail && logged('MALC')) c_alloc_null++;
    }
    printf("%d worlds (%d wild, %d with overflow / divide-by-zero unmasked): %d differ (%d of them wild), %d faulted (%d in both), %d changed bytes outside the footprint, %d replay-only, %d set-ups failed\n",
           iterations, wild_runs, unmasked_runs, differ, wild_differ, faults, fault_both, fp_bad, replay_only, setup_bad);
    printf("per function (worlds / differing):\n");
    for (int i = 0; i < N_KINDS; i++) printf("  %-40s %6d / %d\n", kind_names[i], per[i], per_bad[i]);
    printf("SkidOn: new mark %d, released %d, no mark free %d, degenerate %d, first edge %d, extended %d; GetSkid none free %d\n",
           c_new_mark, c_release, c_skid_full, c_skid_degenerate, c_skid_first, c_skid_extend, c_getskid_none);
    printf("debris_event: burst %d / puff only %d; sparks_event: spark %d / skipped %d\n", c_burst, c_puff_only, c_spark, c_spark_skip);
    printf("splash: handler fresh %d / still on %d; Splash too small %d, capped at 5 m/s ~%d; Update visible %d / hidden %d; 33rd constructor %d\n",
           c_splash_fresh, c_splash_dup, c_splash_small, c_splash_capped, c_splash_vis, c_splash_hidden, c_count32);
    printf("smoke: Update paused %d, expired %d, alpha >= 1 s %d / < 1 s %d; set_smoke_animation frames %d / out of range %d\n",
           c_smoke_paused, c_smoke_expired, c_smoke_hi, c_smoke_lo, c_anim_in, c_anim_out);
    printf("shadow: create_shadow_model 256 cap %d (most faces %d), no model %d; find_idx_of panics %d; IsVisible %d / %d; NULL allocations in %d worlds\n",
           c_cap256, g_max_faces, c_nomodel, c_idx_panic, c_shadow_vis, c_shadow_hid, c_alloc_null);
    return differ || fp_bad ? 1 : 0;
}
