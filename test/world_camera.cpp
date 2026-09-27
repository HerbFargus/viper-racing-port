// world_camera.cpp -- group T2's rewrites (hook/phys_camera.cpp: the camera, the dashboards' keys, the crash sounds,
// the quarter-car rig) against the originals, on random worlds, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_camera.cpp
//        /Fo<dir>\ /Fe<dir>\world_camera.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_camera.exe [worlds] [seed]
//   the fixes (docs/PORTING.md, "Fixes"): the same with /DFIX_TESTS (or /DVP_TEST_FIXES) -- the rewrites as the game builds them; then
//   load_tv_cameras on a camera.tab of more than 32 rows, against the original on its first 32.
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. A world is a
// small arena -- four cars (Car's vtable and the fields the camera reads: frame, velocity, the focus offset at +0x28,
// the cockpit eye, look_back, long_g, over a random body), the output frame, two CollisionSounds and a Sound3D, a
// fake sound manager and UI widget, a QuarterCarControl built by the ORIGINAL constructor -- plus the image's
// statics: PhysTaskFindCar's table, camera_type / focus / physics_paused / the replay flag, and every camera static
// (the chase / camera / blimp frames, the smoothed normal and cos 30, the last focus point and type, the listener
// velocity, the guards) and a TV camera table, filled at random or loaded by the ORIGINAL load_tv_cameras from a
// scripted camera.tab. Stubbed (a jump to a logger): TerrainGetHeight (scripted), SoundSetListener (logs the
// listener and the frame it points at), LogPanic, atexit, ASSERT_MSG (logs the camera's asserts' arguments),
// PhysReplayAddEvent, ResourceExists / StringTableGet / NumRows / GetEntry / Forget (scripted), stricmp and atof
// (the host's), UIDoDialog (logs the dialog, its items and the control, frame-relative) and
// UICustomControl::AddNotification. Everything else runs as the game's code: PhysTaskFindCar, the matrix and vector
// helpers, is_chase / camera_look / CollisionSound::Play(float) / QuarterCarControl's constructor when another
// function calls them, the UIDialogItem constructor, UICustomControl::Dirty, Damper::Setup, QuarterCar::Step and
// the road's TableTerrain methods.
//
// Each world picks a function; the arena and the image's whole .data/.bss are saved, the original runs, the result
// is kept, the world restored, the rewrite runs; the arena, .data/.bss, the return value and the stubs' logs are
// compared, and every byte the original changed must lie in the rewrite's footprint (or a physics static, for the
// physics thread's functions) unless it's replay_only. The main thread's functions run at a random precision
// (24 or 53 bits), the physics thread's at 24. A share of the worlds has floats replaced by extreme values.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(FIX_TESTS) && !defined(VP_TEST_FIXES)
#define VP_TEST_FIXES           // (the flag the other harnesses use)
#endif
#ifndef VP_TEST_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#endif
#include "../hook/port.h"
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { printf("footprint: object() not expected (%s)\n", what); add(obj, 4, what); }
static int g_logf_quiet, g_logf_count;           // the fix test counts the rewrite's log lines instead of printing them
void logf(const char* fmt, ...) { g_logf_count++; if (g_logf_quiet) return; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

#include "../hook/phys_camera.cpp"

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
enum { ARENA_BYTES = 0x10000, CAR_BYTES = 0xeb8, NCARS = 4 };
enum : uint32_t {
    A_CARS = 0x0000,               // 4 x 0x1000
    A_OUT = 0x4000,                // the output frame
    A_PTS = 0x4100,                // camera_look's points
    A_CSND = 0x4200,               // two CollisionSounds (40 bytes)
    A_S3D = 0x4300,                // a Sound3D (64 bytes)
    A_MGR = 0x4400,                // the sound manager (0x100)
    A_WIDGET = 0x4600,             // a UI widget (0x300)
    A_QCC = 0x5000,                // a QuarterCarControl (0x408c)
};
static uint8_t* g_arena;
static uint8_t g_arena_snap[ARENA_BYTES], g_arena_after[ARENA_BYTES];
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* g_data_snap;
static uint8_t* g_data_after;
static uint8_t* car(int i) { return g_arena + A_CARS + 0x1000 * i; }

// the physics and AI statics (a physics-thread function's writes there are saved by the shadow check itself)
struct Global { uint32_t va, size; const char* name; };
static const Global k_globals[] = {
#include "../hook/globals_phys.inc"
};
static bool in_globals(uint32_t a) {
    for (const Global& g : k_globals) if (a >= g.va && a < g.va + g.size) return true;
    return false;
}

// ---- the stubs' log and script ------------------------------------------------------------------------------------
enum { LOG_MAX = 4096 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void log_word(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static void log_bytes(const void* p, int n) { for (int i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, (const uint8_t*)p + i, n - i < 4 ? n - i : 4); log_word(w); } }
static void log_str(const char* s) { size_t n = strlen(s); log_word((uint32_t)n); log_bytes(s, (int)n); }

struct TerrainScript { uint8_t hit; float dy; P3 n; int32_t surface; };
struct Script {
    TerrainScript terrain;
    uint8_t res_exists;
    int rows;
    char cells[40][7][24];
};
static Script g_script;

static uint8_t __cdecl stub_terrain(uint32_t x, uint32_t z, P3* hit, P3* n, int32_t* surface) {
    log_word('TERR'); log_word(x); log_word(z);
    if (!g_script.terrain.hit) return 0;
    hit->x = bitsf(x); hit->z = bitsf(z); hit->y = g_script.terrain.dy;
    *n = g_script.terrain.n;
    *surface = g_script.terrain.surface;
    return 1;
}
static void __cdecl stub_listener(const Listener* l) {
    log_word('LSTN'); log_word((uint32_t)l->focus); log_word((uint32_t)l->frame); log_word((uint32_t)l->velocity);
    log_bytes(l->frame, 48); log_bytes(l->velocity, 12);
}
static void __cdecl stub_panic(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    uint32_t a = va_arg(ap, uint32_t);
    va_end(ap);
    log_word('PANC'); log_word((uint32_t)fmt);
    if ((uint32_t)fmt == 0x004ecc88) log_str((const char*)a); else log_word(a);
}
static int __cdecl stub_atexit(uint32_t fn) { log_word('ATEX'); log_word(fn); return 0; }
static void __cdecl stub_assert(int ok, const char* fmt, ...) {
    log_word('ASRT'); log_word((uint32_t)ok); log_word((uint32_t)fmt);
    if ((uint32_t)fmt == 0x004ecb80 || (uint32_t)fmt == 0x004ecbb8) {
        va_list ap; va_start(ap, fmt);
        log_word(va_arg(ap, uint32_t));
        for (int i = 0; i < 6; i++) log_word(va_arg(ap, uint32_t));
        va_end(ap);
    }
}
static void __cdecl stub_add_event(int type, const void* data, int size) {
    log_word('EVNT'); log_word((uint32_t)type); log_word((uint32_t)size); log_bytes(data, size);
}
static uint8_t __cdecl stub_res_exists(const char* name) { log_word('REXS'); log_str(name); return g_script.res_exists; }
static void* __cdecl stub_st_get(const char* name) { log_word('STGT'); log_str(name); return (void*)0x5ab1e000; }
static int __cdecl stub_st_rows(const void* st) { log_word('STNR'); log_word((uint32_t)st); return g_script.rows; }
static const char* __cdecl stub_st_entry(const void* st, int row, int col) {
    log_word('STEN'); log_word((uint32_t)st); log_word((uint32_t)row); log_word((uint32_t)col);
    if (row < 0 || row >= 40 || col < 0 || col >= 7) return "";
    return g_script.cells[row][col];
}
static void __cdecl stub_st_forget(void* st) { log_word('STFG'); log_word((uint32_t)st); }
static int __cdecl stub_stricmp(const char* a, const char* b) { log_word('SCMP'); log_str(a); log_str(b); return _stricmp(a, b); }
static double __cdecl stub_atof(const char* s) { log_word('ATOF'); log_str(s); return strtod(s, 0); }
static void __fastcall stub_add_notification(void* self, int, int id, const void* p, unsigned size) {
    log_word('NOTE'); log_word((uint32_t)((uint8_t*)self - g_arena)); log_word((uint32_t)id);
    log_word((uint32_t)((const uint8_t*)p - (uint8_t*)self)); log_word(size);
}
// the dialog, relative to its own address (its frame is on the stack, placed differently in each pass)
static uint32_t rel(uint32_t v, uint32_t base) { return v >= base && v < base + 0x4378 ? 0x80000000u | (v - base) : v; }
static int __cdecl stub_do_dialog(const UIDialog* dlg, int w, int h, int x, int y, int ui3d) {
    const uint32_t b = (uint32_t)dlg;
    log_word('DLG!'); log_word(w); log_word(h); log_word(x); log_word(y); log_word(ui3d);
    log_word(dlg->title); log_word(dlg->subtitle); log_word(dlg->_08); log_word(rel((uint32_t)dlg->items, b)); log_word(dlg->idle);
    for (int i = 0; i < 13; i++) for (int k = 0; k < 14; k++) log_word(rel(dlg->items[i].a[k], b));
    // the control, where its constructor made it deterministic (+0x2c / +0x30 come from the stack's garbage)
    const uint8_t* q = (const uint8_t*)b + 0x2ec;
    static const uint32_t words[] = {0x0, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50,
                                     0x54, 0x58, 0x5c, 0x4060, 0x4064, 0x4068};
    for (uint32_t o : words) { uint32_t v; memcpy(&v, q + o, 4); log_word(rel(v, b)); }
    uint32_t h32 = 2166136261u;
    for (int i = 0; i < 0x4000; i++) h32 = (h32 ^ q[0x60 + i]) * 16777619u;
    log_word(h32);
    return 0;
}

// ---- building the world -----------------------------------------------------------------------------------------
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
static void random_frame(Frame* f, const P3& ctr, float spread, bool upright) {
    rotation(&f->rot, upright);
    f->pos.x = ctr.x + range(-spread, spread); f->pos.y = ctr.y + range(-spread * 0.3f, spread * 0.3f); f->pos.z = ctr.z + range(-spread, spread);
}
static P3 v3(float x, float y, float z) { P3 p = {x, y, z}; return p; }

static Frame* const g_cam = (Frame*)S_CAM;
static Frame* const g_chase = (Frame*)S_CHASE;
static Frame* const g_blimp = (Frame*)S_BLIMP;
static TVCamera* const g_tv = (TVCamera*)S_TV;
static void** const g_car_table = (void**)0x00520c18;

static void random_car(int i) {
    uint8_t* c = car(i);
    for (int k = 0; k < CAR_BYTES; k += 4) { float f = range(-100.0f, 100.0f); memcpy(c + k, &f, 4); }
    *(uint32_t*)c = 0x004dbfd0;
    Frame* fr = car_frame(c);
    rotation(&fr->rot, !chance(20));
    fr->pos = v3(range(-2000.0f, 2000.0f), range(-50.0f, 300.0f), range(-2000.0f, 2000.0f));
    int k = rnd() % 10;
    float vs = k < 2 ? 0.5f : k < 3 ? 0.0f : k < 5 ? 8.0f : 80.0f;
    *car_velocity(c) = v3(range(-vs, vs), range(-vs * 0.2f, vs * 0.2f), range(-vs, vs));
    if (chance(30)) { float s = range(-10.0f, 60.0f); *car_velocity(c) = v3(fr->rot.m[6] * s, fr->rot.m[7] * s, fr->rot.m[8] * s); }
    *car_focus_offset(c) = chance(30) ? v3(0, 0, 0) : v3(range(-0.3f, 0.3f), range(-0.5f, 0.5f), range(-1.0f, 1.0f));
    *car_cockpit_eye(c) = chance(30) ? v3(-0.3f, 0.8f, 0.0f) : v3(range(-0.5f, 0.5f), range(0.4f, 1.2f), range(-0.8f, 0.5f));
    c[0x4ec] = chance(20);
    Fat(c, 0xea8) = range(-2.0f, 2.0f);
}

// a camera.tab: rows of type, position, three parameters
static const char* const k_types[] = {"fixed", "pan", "pan_zoom", "chase", "FIXED", "Pan_Zoom", "blimp", ""};
static void script_camera_tab(const P3& ctr) {
    g_script.res_exists = !chance(10);
    int k = rnd() % 10;
    g_script.rows = k < 1 ? 0 : k < 2 ? 1 : k < 9 ? 2 + (int)(rnd() % 12) : 32;
    for (int r = 0; r < g_script.rows; r++) {
        int t = rnd() % 100;
        const char* ty = t < 30 ? k_types[0] : t < 50 ? k_types[1] : t < 70 ? k_types[2] : t < 90 ? k_types[3] : k_types[4 + rnd() % 4];
        strcpy(g_script.cells[r][0], ty);
        sprintf(g_script.cells[r][1], "%.3f", ctr.x + range(-800.0f, 800.0f));
        sprintf(g_script.cells[r][2], "%.3f", ctr.y + range(-20.0f, 80.0f));
        sprintf(g_script.cells[r][3], "%.3f", ctr.z + range(-800.0f, 800.0f));
        if (!strcmp(ty, "fixed")) {
            sprintf(g_script.cells[r][4], "%.2f", chance(15) ? 0.0f : range(-180.0f, 180.0f));
            sprintf(g_script.cells[r][5], "%.2f", chance(15) ? 0.0f : range(-90.0f, 90.0f));
            sprintf(g_script.cells[r][6], "%.2f", chance(50) ? 0.0f : range(-30.0f, 30.0f));
        } else {
            sprintf(g_script.cells[r][4], "%.4f", range(-3.0f, 30.0f));
            sprintf(g_script.cells[r][5], "%.3f", range(0.0f, 100.0f));
            sprintf(g_script.cells[r][6], "%g", chance(80) ? 0.0f : range(-1.0f, 1.0f));
        }
        if (chance(3)) sprintf(g_script.cells[r][1 + rnd() % 6], "%s", chance(50) ? "1e40" : "junk");
    }
}

static void random_tvs(const P3& ctr) {
    int k = rnd() % 10;
    int n = k < 1 ? 0 : k < 2 ? 1 : k < 9 ? 2 + (int)(rnd() % 10) : 32;
    *(int32_t*)S_NUM_TV = n;
    for (int i = 0; i < 32; i++) {
        TVCamera& c = g_tv[i];
        int t = rnd() % 100;
        c.type = t < 30 ? 0 : t < 50 ? 1 : t < 70 ? 2 : t < 95 ? 3 : (int)(rnd() % 8);
        c.pos = v3(ctr.x + range(-1500.0f, 1500.0f), ctr.y + range(-20.0f, 80.0f), ctr.z + range(-1500.0f, 1500.0f));
        if (chance(3)) c.pos = v3(ctr.x, ctr.y + range(1.0f, 30.0f), ctr.z);       // right above the car
        if (c.type == 0) {
            c.a = chance(10) ? 0.0f : range(-180.0f, 180.0f); c.b = chance(10) ? 0.0f : range(-90.0f, 90.0f);
            c.c = chance(40) ? 0.0f : range(-30.0f, 30.0f);
            if (chance(5)) c.a = c.b = c.c = 0.0f;                                   // no rotation: identity
            if (chance(3)) { c.a = range(-0.003f, 0.003f); c.b = c.c = 0.0f; }      // under 1e-4 rad
        } else {
            c.a = range(-3.0f, 30.0f); c.b = range(0.0f, 100.0f); c.c = range(-1.0f, 1.0f);
        }
    }
    *(int32_t*)S_CUR_TV = chance(80) ? (int)(rnd() % 32) : (int)(rnd() % 40) - 4;
}

// the function-static and camera state, and the focus
static int g_focus_car;
static void random_statics() {
    for (int i = 0; i < NCARS; i++) random_car(i);
    for (int i = 0; i < 16; i++) g_car_table[i] = chance(85) ? car(rnd() % NCARS) : 0;
    *(uint32_t*)0x00520bb4 = chance(95) ? 0x00123450 : 0;                  // the phob list (PhysTaskFindCar asserts)
    int32_t focus = (int32_t)(rnd() % 16);
    *(int32_t*)S_FOCUS = focus;
    uint8_t* fc = (uint8_t*)g_car_table[focus];
    P3 ctr = fc ? car_frame(fc)->pos : v3(0, 0, 0);
    int ty = (int)(rnd() % 100);
    int32_t type = ty < 97 ? (int32_t)(rnd() % 13) : (int32_t)(chance(50) ? 13 + rnd() % 100 : 0x80000000u | rnd());
    if (chance(15)) type = 10 + (rnd() % 2) * 2;                              // more TV
    *(int32_t*)S_CAMERA_TYPE = type;
    *(int32_t*)S_LAST_TYPE = chance(60) ? type : (int32_t)(rnd() % 13);
    uint8_t guard = chance(85) ? 7 : (uint8_t)(rnd() % 8);
    if (chance(3)) guard |= 0xf8 & rnd();
    *(uint8_t*)S_GUARD = guard;
    P3* last = (P3*)S_LAST_POS;
    int lk = rnd() % 10;
    *last = lk < 5 ? v3(ctr.x + range(-0.5f, 0.5f), ctr.y + range(-0.5f, 0.5f), ctr.z + range(-0.5f, 0.5f))
          : lk < 8 ? v3(ctr.x + range(-50.0f, 50.0f), ctr.y + range(-5.0f, 5.0f), ctr.z + range(-50.0f, 50.0f))
                   : v3(range(-2000.0f, 2000.0f), range(-50.0f, 300.0f), range(-2000.0f, 2000.0f));
    P3* S = (P3*)S_SMOOTH_UP;
    *S = v3(range(-0.3f, 0.3f), range(0.8f, 1.0f), range(-0.3f, 0.3f));
    *(float*)S_COS30 = (guard & 4) ? bitsf(0x3f5db3d7) : range(-1.0f, 1.0f);
    random_frame(g_chase, ctr, 50.0f, !chance(20));
    random_frame(g_cam, ctr, 100.0f, !chance(20));
    if (chance(5)) g_cam->pos = v3(ctr.x + range(-0.3f, 0.3f), ctr.y, ctr.z + range(-0.3f, 0.3f));
    if (chance(5)) g_cam->pos = v3(ctr.x + range(-0.005f, 0.005f), ctr.y + range(5.0f, 50.0f), ctr.z + range(-0.005f, 0.005f));
    *(P3*)S_CAM_VEL = v3(range(-30.0f, 30.0f), range(-3.0f, 3.0f), range(-30.0f, 30.0f));
    random_frame(g_blimp, ctr, 300.0f, !chance(30));
    random_tvs(ctr);
    *(uint8_t*)S_PAUSED = chance(15);
    *(uint8_t*)0x005218e8 = chance(30);                                     // PhysReplayPlayMode
    *(void**)S_SOUND_MANAGER = chance(90) ? g_arena + A_MGR : 0;
    // the terrain under the focus point
    TerrainScript& t = g_script.terrain;
    t.hit = chance(75);
    t.dy = ctr.y - range(0.2f, 1.0f);
    int nk = rnd() % 10;
    double nx, ny, nz;
    const float* up = fc ? &car_frame(fc)->rot.m[3] : 0;
    if (nk < 4 && up) { nx = up[0] + range(-0.2f, 0.2f); ny = up[1] + range(-0.2f, 0.2f); nz = up[2] + range(-0.2f, 0.2f); }
    else if (nk < 7) { nx = range(-0.4f, 0.4f); nz = range(-0.4f, 0.4f); ny = 1.0; }
    else if (nk < 9) { nx = range(-1.0f, 1.0f); nz = range(-1.0f, 1.0f); ny = range(-1.0f, 1.0f); }
    else { nx = 0; ny = 1; nz = 0; }
    double l = sqrt(nx * nx + ny * ny + nz * nz);
    if (l > 1e-6) { nx /= l; ny /= l; nz /= l; }
    t.n = v3((float)nx, (float)ny, (float)nz);
    t.surface = (int32_t)(rnd() % 30);
    script_camera_tab(ctr);
    g_focus_car = fc ? (int)((fc - car(0)) / 0x1000) : -1;
}

// the rest of the arena: sounds, the control, the widget; the end item the dialog copies
static void random_parts() {
    for (int i = 0; i < 2; i++) {
        uint8_t* s = g_arena + A_CSND + 0x40 * i;
        for (int k = 0; k < 40; k += 4) { float f = range(-50.0f, 50.0f); memcpy(s + k, &f, 4); }
        *(uint8_t**)(s + 0xc) = chance(80) ? g_arena + A_S3D : 0;
    }
    uint8_t* s3 = g_arena + A_S3D;
    for (int k = 0; k < 64; k++) s3[k] = (uint8_t)rnd();
    *(uint32_t*)s3 = 0x004dd6d8;
    Fat(s3, 8) = chance(40) ? 1.0f : uni();
    uint8_t* w = g_arena + A_WIDGET;
    for (int k = 0; k < 0x300; k++) w[k] = (uint8_t)rnd();
    w[0x16] = chance(50);
    for (int k = 0; k < 56; k++) ((uint8_t*)S_ITEM_END)[k] = (uint8_t)rnd();
    *(uint8_t*)0x004ecce4 = (uint8_t)(chance(50) ? rnd() % 2 : rnd());
    *(uint8_t*)0x004ecce8 = (uint8_t)(chance(50) ? rnd() % 2 : rnd());
}

// a quarter-car rig: the ORIGINAL constructor, then random settings and state
typedef uint8_t*(__fastcall* QccCtor)(uint8_t*, int);
static void random_qcc(bool construct) {
    uint8_t* q = g_arena + A_QCC;
    for (int k = 0; k < 0x408c; k += 4) { float f = range(-500.0f, 500.0f); memcpy(q + k, &f, 4); }
    Fat(q, 0x18) = range(100.0f, 500.0f); Fat(q, 0x1c) = range(10.0f, 60.0f);
    Fat(q, 0x20) = range(1e4f, 1e5f); Fat(q, 0x24) = range(1e5f, 6e5f);
    if (chance(10)) wild_at(q + 0x18 + 4 * (rnd() % 4));
    if (!construct) return;
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    ((QccCtor)0x00447320)(q, 0);
    Fat(q, 0x18) = range(100.0f, 500.0f); Fat(q, 0x1c) = range(10.0f, 60.0f);
    Fat(q, 0x20) = range(1e4f, 1e5f); Fat(q, 0x24) = range(1e5f, 6e5f);
    Fat(q, 0x4060) = range(0.0f, 300.0f); Fat(q, 0x4064) = range(5.0f, 300.0f); Fat(q, 0x4068) = range(5.0f, 300.0f);
    Fat(q, 0x406c) = chance(50) ? 1e30f : range(0.0f, 1.0f);
    Fat(q, 0x58) = range(0.0f, 1000.0f);                                    // the road's position
    *(uint8_t**)(q + 0x14) = chance(70) ? g_arena + A_WIDGET : 0;
    q[0x4088] = (uint8_t)(chance(50) ? rnd() % 2 : rnd());
    if (chance(8)) wild_at(q + (chance(50) ? 0x4064 + 4 * (rnd() % 2) : 0x18 + 4 * (rnd() % 4)));
}

// ---- the functions under test --------------------------------------------------------------------------------------
enum Kind { K_UPDATE, K_LOOK, K_CHASE, K_BLIMP, K_LOAD, K_TVCTOR, K_CAMKEY, K_PHYSKEY, K_PLAYF, K_PLAY, K_QCTOR, K_QCREATE,
            K_QCALLBACK, K_QGRAPH, K_QTEST, N_KINDS };
static const char* const kind_names[N_KINDS] = {"update_camera", "camera_look", "is_chase", "blimp_to_tv", "load_tv_cameras",
    "TVCamera::TVCamera", "CameraDashboardKey", "PhysicsDashboardKey", "CollisionSound::Play(float)", "CollisionSound::Play",
    "QuarterCarControl::QuarterCarControl", "QuarterCarControl::Create", "QuarterCarControl::Callback",
    "QuarterCarControl::graph_fn", "QuarterCarTest"};
static const int kind_weight[N_KINDS] = {400, 60, 10, 20, 20, 3, 3, 10, 20, 20, 1, 3, 5, 3, 1};
static const bool main_thread[N_KINDS] = {false, false, false, true, false, false, true, true, false, false, true, true, true, true, true};

struct Args { int i; uint16_t key; uint32_t f; P3* a; P3* b; uint8_t* self; };
static Args g_args;

static void random_args(Kind k) {
    g_args.i = chance(90) ? (int)(rnd() % 12) : (int)rnd();
    g_args.key = (uint16_t)(chance(40) ? 0x63 : chance(40) ? 0x77 : chance(50) ? (0x6300 | 0x63) : rnd());
    P3* pa = (P3*)(g_arena + A_PTS);
    P3* pb = (P3*)(g_arena + A_PTS + 16);
    g_args.a = pa; g_args.b = pb;
    if (k == K_LOOK) {
        g_args.a = chance(70) ? &g_cam->pos : pa;
        P3 e = *g_args.a;
        if (g_args.a == pa) { *pa = v3(range(-2000.0f, 2000.0f), range(-50.0f, 300.0f), range(-2000.0f, 2000.0f)); e = *pa; }
        int t = rnd() % 10;
        *pb = t < 6 ? v3(e.x + range(-60.0f, 60.0f), e.y + range(-10.0f, 10.0f), e.z + range(-60.0f, 60.0f))
            : t < 7 ? v3(e.x + range(-0.5f, 0.5f), e.y + range(-0.5f, 0.5f), e.z + range(-0.5f, 0.5f))       // within 1 m
            : t < 9 ? v3(e.x + range(-0.009f, 0.009f), e.y + range(-40.0f, 40.0f), e.z + range(-0.009f, 0.009f))   // straight up / down
                    : v3(range(-20000.0f, 20000.0f), range(-9000.0f, 9000.0f), range(-20000.0f, 20000.0f));
        if (chance(3)) g_args.b = g_args.a;                                  // the eye itself
        if (chance(10)) wild_at((float*)g_args.b + rnd() % 3);
        if (chance(5)) wild_at((float*)g_args.a + rnd() % 3);
    }
    if (k == K_PLAYF || k == K_PLAY) {
        g_args.self = g_arena + A_CSND + 0x40 * (rnd() % 2);
        *pa = v3(range(-2000.0f, 2000.0f), range(-50.0f, 300.0f), range(-2000.0f, 2000.0f));
        int t = rnd() % 10;
        float vs = t < 3 ? 100.0f : t < 7 ? 5000.0f : 30000.0f;
        *pb = v3(range(-vs, vs), range(-vs, vs), range(-vs, vs));
        const float cur = Fat(g_arena + A_S3D, 8);
        g_args.f = fbits(chance(30) ? cur : chance(50) ? uni() : range(0.0f, 2.0f));
        if (chance(10)) g_args.f = fbits(wildf());
        if (chance(8)) wild_at((float*)pb + rnd() % 3);
    }
    // (not an infinite or huge speed: the road's wrap loop never ends -- in the original too)
    if (k == K_QGRAPH) {
        g_args.f = fbits(chance(90) ? range(0.0f, 200.0f) : wildf());
        if (!(fabs(bitsf(g_args.f)) < 1e4f) && bitsf(g_args.f) == bitsf(g_args.f)) g_args.f = fbits(range(-5.0f, 5.0f));
    }
    if (k == K_QCALLBACK) g_args.i = (int)rnd();
}

// some floats extreme, for the camera
static void wildify_camera() {
    int n = 1 + rnd() % 3;
    for (int i = 0; i < n; i++) {
        int w = rnd() % 10;
        uint8_t* fc = (uint8_t*)g_car_table[*(int32_t*)S_FOCUS];
        if (w < 3 && fc) {
            static const uint32_t offs[] = {0x28, 0x2c, 0x30, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x64,
                                            0x234, 0x238, 0x23c, 0x47c, 0x480, 0x484, 0xea8};
            wild_at(fc + offs[rnd() % (sizeof offs / sizeof *offs)]);
        } else if (w < 5) wild_at((float*)g_chase + rnd() % 12);
        else if (w < 6) wild_at((float*)g_cam + rnd() % 12);
        else if (w < 7) wild_at((float*)S_SMOOTH_UP + rnd() % 3);
        else if (w < 8) wild_at((float*)S_LAST_POS + rnd() % 3);
        else if (w < 9) wild_at((float*)&g_tv[rnd() % 32] + 1 + rnd() % 6);
        else if (chance(50)) wild_at(&g_script.terrain.n.x + rnd() % 3); else wild_at((float*)g_blimp + rnd() % 12);
    }
}

typedef int(__cdecl* UpdateCam_t)(Frame*);
typedef void(__cdecl* Look_t2)(const P3*, const P3*);
typedef uint8_t(__cdecl* IsChase_t2)(int);
typedef void(__cdecl* Void_t)();
typedef void*(__fastcall* ThisRet_t)(void*, int);
typedef uint8_t(__cdecl* Key_t)(uint16_t);
typedef void(__fastcall* PlayF_t2)(void*, int, const P3*, uint32_t);
typedef void(__fastcall* Play_t2)(void*, int, const P3*, const P3*);
typedef void(__fastcall* ThisVoid_t2)(void*, int);
typedef void(__fastcall* Callback_t)(void*, int, int, const void*);
typedef float(__cdecl* Graph_t)(uint32_t, uint8_t*);                    // the float argument pushed as bits

static uint32_t call_kind(Kind k, bool rw) {
    uint32_t r = 0;
    uint8_t* q = g_arena + A_QCC;
    switch (k) {
    case K_UPDATE: r = (uint32_t)(rw ? update_camera((Frame*)(g_arena + A_OUT)) : ((UpdateCam_t)0x00427610)((Frame*)(g_arena + A_OUT))); break;
    case K_LOOK: rw ? camera_look(g_args.a, g_args.b) : ((Look_t2)0x00428790)(g_args.a, g_args.b); break;
    case K_CHASE: r = rw ? is_chase(g_args.i) : ((IsChase_t2)0x00428750)(g_args.i); break;
    case K_BLIMP: rw ? blimp_to_tv() : ((Void_t)0x004267b0)(); break;
    case K_LOAD: rw ? load_tv_cameras() : ((Void_t)0x00428db0)(); break;
    case K_TVCTOR: r = (uint32_t)(rw ? (void*)TVCamera_ctor((TVCamera*)(g_arena + A_PTS), 0) : ((ThisRet_t)0x004290f0)(g_arena + A_PTS, 0)); break;
    case K_CAMKEY: r = rw ? CameraDashboardKey(g_args.key) : ((Key_t)0x004290e0)(g_args.key); break;
    case K_PHYSKEY: r = rw ? PhysicsDashboardKey(g_args.key) : ((Key_t)0x00429ba0)(g_args.key); break;
    case K_PLAYF: rw ? CollisionSound_PlayF(g_args.self, 0, g_args.a, g_args.f) : ((PlayF_t2)0x0043c510)(g_args.self, 0, g_args.a, g_args.f); break;
    case K_PLAY: rw ? CollisionSound_Play(g_args.self, 0, g_args.a, g_args.b) : ((Play_t2)0x0043c560)(g_args.self, 0, g_args.a, g_args.b); break;
    case K_QCTOR: r = (uint32_t)(rw ? (void*)QuarterCarControl_ctor(q, 0) : ((ThisRet_t)0x00447320)(q, 0)); break;
    case K_QCREATE: rw ? QuarterCarControl_Create(q, 0) : ((ThisVoid_t2)0x004475b0)(q, 0); break;
    case K_QCALLBACK: rw ? QuarterCarControl_Callback(q, 0, g_args.i, g_arena) : ((Callback_t)0x00447600)(q, 0, g_args.i, g_arena); break;
    case K_QGRAPH: {
        float f = rw ? ((Graph_t)&QuarterCarControl_graph_fn)(g_args.f, q) : ((Graph_t)0x004478b0)(g_args.f, q);
        r = fbits(f);
        break;
    }
    case K_QTEST: rw ? QuarterCarTest() : ((Void_t)0x00446b40)(); break;
    default: break;
    }
    if (k == K_CHASE || k == K_CAMKEY || k == K_PHYSKEY) r &= 0xff;     // an unsigned char in al
    return r;
}
static uint32_t g_fault_eip, g_fault_code, g_fault_addr;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_eip = (uint32_t)e->ContextRecord->Eip;
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_addr = e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static unsigned g_pc;
static int run_guarded(Kind k, bool rw, uint32_t* ret) {
    g_log.n = 0;
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, g_pc, _MCW_PC);
    int fault = 0;
    __try { *ret = call_kind(k, rw); } __except (fault_filter(GetExceptionInformation())) { fault = 1; }
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    return fault;
}

static void footprint_of(Kind k, Footprint& fp) {
    fp.n = 0; fp.pure = false; fp.replay_only = 0;
    uint8_t* q = g_arena + A_QCC;
    switch (k) {
    case K_UPDATE: fpof_update_camera(fp, (Frame*)(g_arena + A_OUT)); break;
    case K_LOOK: fpof_camera_look(fp, g_args.a, g_args.b); break;
    case K_CHASE: fpof_is_chase(fp, g_args.i); break;
    case K_BLIMP: fpof_blimp_to_tv(fp); break;
    case K_LOAD: fpof_load_tv_cameras(fp); break;
    case K_TVCTOR: fpof_TVCamera_ctor(fp, (TVCamera*)(g_arena + A_PTS), 0); break;
    case K_CAMKEY: fpof_CameraDashboardKey(fp, g_args.key); break;
    case K_PHYSKEY: fpof_PhysicsDashboardKey(fp, g_args.key); break;
    case K_PLAYF: fpof_CollisionSound_PlayF(fp, g_args.self, 0, g_args.a, g_args.f); break;
    case K_PLAY: fpof_CollisionSound_Play(fp, g_args.self, 0, g_args.a, g_args.b); break;
    case K_QCTOR: fpof_QuarterCarControl_ctor(fp, q, 0); break;
    case K_QCREATE: fpof_QuarterCarControl_Create(fp, q, 0); break;
    case K_QCALLBACK: fpof_QuarterCarControl_Callback(fp, q, 0, g_args.i, g_arena); break;
    case K_QGRAPH: fpof_QuarterCarControl_graph_fn(fp, bitsf(g_args.f), q); break;
    case K_QTEST: fpof_QuarterCarTest(fp); break;
    default: break;
    }
}
static bool in_footprint(const Footprint& fp, const uint8_t* p) {
    for (int i = 0; i < fp.n; i++)
        if (p >= (const uint8_t*)fp.r[i].p && p < (const uint8_t*)fp.r[i].p + fp.r[i].n) return true;
    return false;
}

static const char* where(uint32_t a, char* buf) {
    struct N { uint32_t at, n; const char* name; };
    static const N names[] = {{S_SMOOTH_UP, 12, "normal"}, {S_CHASE, 48, "chase"}, {S_COS30, 4, "cos30"}, {S_LAST_POS, 12, "last_pos"},
        {S_CAM, 48, "camera"}, {S_GUARD, 1, "guard"}, {S_TV, 896, "tv"}, {S_BLIMP, 48, "blimp"}, {S_NUM_TV, 4, "num_tv"},
        {S_LAST_TYPE, 4, "last_type"}, {S_CAM_VEL, 12, "cam_vel"}, {S_CUR_TV, 4, "cur_tv"}};
    uint32_t base = (uint32_t)(uintptr_t)g_arena;
    if (a >= base && a < base + ARENA_BYTES) { sprintf(buf, "arena+0x%x", a - base); return buf; }
    for (const N& n : names) if (a >= n.at && a < n.at + n.n) { sprintf(buf, "%s+0x%x", n.name, a - n.at); return buf; }
    sprintf(buf, "0x%08x", a);
    return buf;
}

// ---- main ----------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 100000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_camera.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    patch_jmp(0x00465c40, (void*)&stub_terrain);
    patch_jmp(0x00471e00, (void*)&stub_listener);
    patch_jmp(0x004112b0, (void*)&stub_panic);
    patch_jmp(0x004ceff0, (void*)&stub_atexit);
    patch_jmp(0x00426d70, (void*)&stub_assert);
    patch_jmp(0x0042d8b0, (void*)&stub_add_event);
    patch_jmp(0x00419d10, (void*)&stub_res_exists);
    patch_jmp(0x0041b210, (void*)&stub_st_get);
    patch_jmp(0x0041b290, (void*)&stub_st_rows);
    patch_jmp(0x0041b2a0, (void*)&stub_st_entry);
    patch_jmp(0x0041b270, (void*)&stub_st_forget);
    patch_jmp(0x004da350, (void*)&stub_stricmp);
    patch_jmp(0x004cfe40, (void*)&stub_atof);
    patch_jmp(0x00478ff0, (void*)&stub_do_dialog);
    patch_jmp(0x0047e840, (void*)&stub_add_notification);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_data_snap = (uint8_t*)malloc(DATA_BYTES);
    g_data_after = (uint8_t*)malloc(DATA_BYTES);

    static Footprint fp;
    const bool g_trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    int differ = 0, faults = 0, fp_bad = 0, replay_only = 0, wild_runs = 0, wild_differ = 0;
    int per[N_KINDS] = {0}, per_bad[N_KINDS] = {0};
    // coverage (update_camera): per camera type with a focus car; branches
    int c_type[14] = {0}, c_nocar = 0, c_panic = 0, c_reset = 0, c_atexit = 0, c_hit = 0, c_miss = 0, c_slow = 0,
        c_paused = 0, c_side = 0, c_lookback = 0, c_notv = 0, c_tv_near = 0, c_tv0 = 0, c_tvtype[5] = {0}, c_identity = 0,
        c_fallback = 0, c_loaded = 0, c_look_early = 0, c_look_ok = 0, c_play_nosnd = 0, c_play_vol = 0, c_q_best = 0,
        c_load_panic = 0;
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weight[i];
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), kk = 0;
        while (pick >= kind_weight[kk]) pick -= kind_weight[kk++];
        const Kind kind = (Kind)kk;
        memset(g_arena, 0, ARENA_BYTES);
        random_statics();
        random_parts();
        random_qcc(kind == K_QCREATE || kind == K_QCALLBACK || kind == K_QGRAPH);
        bool loaded = false;
        if (kind == K_UPDATE && chance(20)) {                  // the TV cameras from camera.tab, by the original
            g_log.n = 0;
            unsigned cw;
            _controlfp_s(&cw, _PC_24, _MCW_PC);
            __try { ((Void_t)0x00428db0)(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (*(int32_t*)S_NUM_TV > 32) *(int32_t*)S_NUM_TV = 32;
            loaded = true;
            c_loaded++;
        }
        random_args(kind);
        bool wild = kind == K_UPDATE && chance(12);
        if (wild) { wildify_camera(); wild_runs++; }
        g_pc = main_thread[kind] && chance(50) ? _PC_53 : _PC_24;
        if (g_trace) printf("world %d: %s\n", it, kind_names[kind]);
        footprint_of(kind, fp);
        if (fp.replay_only) replay_only++;
        // run both
        memcpy(g_arena_snap, g_arena, ARENA_BYTES);
        memcpy(g_data_snap, DATA, DATA_BYTES);
        uint32_t ro = 0, rn = 0;
        int fo = run_guarded(kind, false, &ro);
        memcpy(g_arena_after, g_arena, ARENA_BYTES);
        memcpy(g_data_after, DATA, DATA_BYTES);
        g_log_orig = g_log;
        memcpy(g_arena, g_arena_snap, ARENA_BYTES);
        memcpy(DATA, g_data_snap, DATA_BYTES);
        int fn = run_guarded(kind, true, &rn);
        per[kind]++;
        if (fo || fn) {
            faults++;
            if (fo != fn || g_fault_code != 0) {
                if (faults <= 5 || fo != fn)
                    printf("  world %d (%s): %s / %s; fault %08x at %08x, address %08x\n", it, kind_names[kind], fo ? "original faulted" : "original ran",
                           fn ? "rewrite faulted" : "rewrite ran", g_fault_code, g_fault_eip, g_fault_addr);
            }
            if (fo != fn) { differ++; per_bad[kind]++; }
            continue;
        }
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
            printf("    return: original %08x rewrite %08x\n", ro, rn);
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
            if (kind == K_UPDATE) printf("    (camera type %d, focus car %d, wild %d, pc %s)\n", *(int32_t*)(g_data_snap + (S_CAMERA_TYPE - 0x4e1000)), g_focus_car, wild, g_pc == _PC_24 ? "24" : "53");
            if (differ >= 20) { printf("stopping after 20 differing worlds\n"); break; }
        }
        // the original's writes against the footprint
        if (!fp.replay_only) {
            const bool phys = !main_thread[kind];
            bool bad = false;
            for (uint32_t i = 0; i < ARENA_BYTES && !bad; i++)
                if (g_arena_after[i] != g_arena_snap[i] && !in_footprint(fp, g_arena + i)) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, kind_names[kind], where((uint32_t)(uintptr_t)(g_arena + i), buf));
                    bad = true;
                }
            for (uint32_t i = 0; i < DATA_BYTES && !bad; i++)
                if (g_data_after[i] != g_data_snap[i] && !in_footprint(fp, DATA + i) && !(phys && in_globals(0x4e1000 + i))) {
                    printf("  FOOTPRINT world %d (%s): %s changed outside it\n", it, kind_names[kind], where(0x4e1000 + i, buf));
                    bad = true;
                }
            fp_bad += bad;
        }
        // coverage, from the starting state and the original's log
        auto snap = [&](uint32_t a) { return g_data_snap + (a - 0x4e1000); };
        auto after = [&](uint32_t a) { return g_data_after + (a - 0x4e1000); };
        auto logged = [&](uint32_t tag) { for (uint32_t i = 0; i < g_log_orig.n && i < LOG_MAX; i++) if (g_log_orig.w[i] == tag) return true; return false; };
        if (kind == K_UPDATE) {
            const int32_t type = *(int32_t*)snap(S_CAMERA_TYPE);
            const bool has_car = g_focus_car >= 0;
            if (!has_car) c_nocar++;
            else if ((uint32_t)type > 12) c_type[13]++, c_panic += logged('PANC');
            else {
                c_type[type]++;
                c_atexit += logged('ATEX');
                if (type == 0) (g_script.terrain.hit ? c_hit : c_miss)++;
                const int32_t last = *(int32_t*)snap(S_LAST_TYPE);
                if (type >= 4 && type <= 7) {
                    const P3 v = *car_velocity(car(g_focus_car));
                    if ((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z < 1.0) c_slow++;
                    if (*snap(S_PAUSED)) c_paused++;
                    if (type == 7 || car(g_focus_car)[0x4ec]) c_lookback++;
                    // a sideways-swing step: the camera's final position differs from a plain chase (can't see; count
                    // fast and unpaused worlds)
                    if (!*snap(S_PAUSED) && (double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z >= 1.0) c_side++;
                }
                if (type == 10 || type == 12) {
                    const int32_t num = *(int32_t*)snap(S_NUM_TV);
                    if (num <= 1) c_notv++;
                    else {
                        (type == 10 ? c_tv_near : c_tv0)++;
                        const int32_t cur = *(int32_t*)after(S_CUR_TV);
                        if (cur >= 0 && cur < 32) {
                            const TVCamera& c = ((const TVCamera*)snap(S_TV))[cur];
                            c_tvtype[c.type >= 0 && c.type <= 3 ? c.type : 4]++;
                            if (c.type == 0 && fabs(c.a) < 0.006 && fabs(c.b) < 0.006 && fabs(c.c) < 0.006) c_identity++;
                            const P3 fp3 = car_frame(car(g_focus_car))->pos;
                            if (c.type != 0 && fabs(c.pos.x - fp3.x) < 0.5 && fabs(c.pos.z - fp3.z) < 0.5) c_fallback++;
                        }
                    }
                }
                // a teleport reset: moved 2 m at under 20 m/s, chase camera, type unchanged
                {
                    const P3 lp = *(const P3*)snap(S_LAST_POS);
                    const P3 fp3 = car_frame(car(g_focus_car))->pos;
                    const P3 v = *car_velocity(car(g_focus_car));
                    double dd = ((double)lp.x - fp3.x) * (lp.x - fp3.x) + ((double)lp.y - fp3.y) * (lp.y - fp3.y) + ((double)lp.z - fp3.z) * (lp.z - fp3.z);
                    if (dd > 6.0 && (double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z < 390.0 && type == last && type >= 4 && type <= 6) c_reset++;
                }
            }
        }
        if (kind == K_LOOK) (logged('ASRT') && memcmp(snap(S_CAM), after(S_CAM), 36) ? c_look_ok : c_look_early)++;
        if (kind == K_LOAD && logged('PANC')) c_load_panic++;
        if ((kind == K_PLAY || kind == K_PLAYF) && !*(void**)(g_args.self + 0xc)) c_play_nosnd++;
        if ((kind == K_PLAY || kind == K_PLAYF) && g_arena_after[A_S3D + 0x2c] != g_arena_snap[A_S3D + 0x2c]) c_play_vol++;
        if (kind == K_QGRAPH && memcmp(g_arena_after + A_QCC + 0x406c, g_arena_snap + A_QCC + 0x406c, 4)) c_q_best++;
        (void)loaded;
    }
    printf("%d worlds (%d wild): %d differ (%d of them wild), %d faulted, %d changed bytes outside the footprint, %d replay-only\n",
           iterations, wild_runs, differ, wild_differ, faults, fp_bad, replay_only);
    printf("per function (worlds / differing):");
    for (int i = 0; i < N_KINDS; i++) printf("%s %s %d/%d", i ? "," : "", kind_names[i], per[i], per_bad[i]);
    printf("\nupdate_camera per type (with a focus car):");
    for (int i = 0; i < 13; i++) printf(" %d:%d", i, c_type[i]);
    printf(" out-of-range:%d (panics %d); no focus car %d\n", c_type[13], c_panic, c_nocar);
    printf("  atexit inits %d, cockpit terrain hit %d / miss %d, teleport resets ~%d, chase slow %d / paused %d / swing %d / look back %d\n",
           c_atexit, c_hit, c_miss, c_reset, c_slow, c_paused, c_side, c_lookback);
    printf("  TV: none %d, nearest %d, camera 0 %d; types fixed %d pan %d pan_zoom %d chase %d other %d; identity ~%d, straight-down ~%d; loaded from camera.tab %d\n",
           c_notv, c_tv_near, c_tv0, c_tvtype[0], c_tvtype[1], c_tvtype[2], c_tvtype[3], c_tvtype[4], c_identity, c_fallback, c_loaded);
    printf("others: camera_look turned %d / left %d, load_tv_cameras unknown types %d, crash sound without a Sound3D %d, volume changed %d, graph new best %d\n",
           c_look_ok, c_look_early, c_load_panic, c_play_nosnd, c_play_vol, c_q_best);
#ifdef VP_TEST_FIXES
    // load_tv_cameras' FIX: a camera.tab of 33..40 rows must load exactly as the original loads its first 32 rows
    // (the stub calls included: the rows past 32 are never read), the count 32 and nothing past the table written.
    // The original on the long table, for the record, overruns (the TimerConditioner at 0x521038, or a fault).
    int fix_bad = 0, orig_overran = 0;
    g_logf_quiet = 1;
    g_logf_count = 0;
    const int fix_runs = 2000;
    for (int it = 0; it < fix_runs; it++) {
        memset(g_arena, 0, ARENA_BYTES);
        random_statics();
        g_script.res_exists = 1;
        for (int r = 0; r < 40; r++) {
            strcpy(g_script.cells[r][0], k_types[rnd() % 6]);
            for (int c = 1; c < 7; c++) sprintf(g_script.cells[r][c], "%.3f", range(-500.0f, 500.0f));
        }
        const int rows = 33 + (int)(rnd() % 8);
        g_pc = _PC_24;
        memcpy(g_arena_snap, g_arena, ARENA_BYTES);
        memcpy(g_data_snap, DATA, DATA_BYTES);
        uint32_t ro = 0, rn = 0;
        g_script.rows = 32;
        int fo = run_guarded(K_LOAD, false, &ro);
        memcpy(g_arena_after, g_arena, ARENA_BYTES);
        memcpy(g_data_after, DATA, DATA_BYTES);
        g_log_orig = g_log;
        memcpy(g_arena, g_arena_snap, ARENA_BYTES);
        memcpy(DATA, g_data_snap, DATA_BYTES);
        g_script.rows = rows;
        int fn = run_guarded(K_LOAD, true, &rn);
        bool ok = !fo && !fn && *(int32_t*)S_NUM_TV == 32 && !memcmp(g_arena_after, g_arena, ARENA_BYTES) &&
                  !memcmp(g_data_after, DATA, DATA_BYTES) && g_log.n == g_log_orig.n &&
                  !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)) &&
                  !memcmp(DATA + (0x00521038 - 0x4e1000), g_data_snap + (0x00521038 - 0x4e1000), 0x521084 - 0x521038);
        if (!ok && fix_bad++ < 4) printf("  FIX load_tv_cameras (%d rows, run %d): differs from the original's first 32 rows\n", rows, it);
        if (it < 20) {                                          // the original on the long table
            memcpy(g_arena, g_arena_snap, ARENA_BYTES);
            memcpy(DATA, g_data_snap, DATA_BYTES);
            int f = run_guarded(K_LOAD, false, &ro);
            if (f || memcmp(DATA + (0x00521038 - 0x4e1000), g_data_snap + (0x00521038 - 0x4e1000), 28)) orig_overran++;
        }
        memcpy(g_arena, g_arena_snap, ARENA_BYTES);
        memcpy(DATA, g_data_snap, DATA_BYTES);
    }
    printf("fix build: load_tv_cameras on a camera.tab of 33..40 rows %d times: %d differ from the original on the first 32"
           " (the original itself overran %d of 20); %d log lines\n", fix_runs, fix_bad, orig_overran, g_logf_count);
    if (fix_bad || orig_overran != 20 || g_logf_count != fix_runs) differ++;
#endif
    return differ || fp_bad ? 1 : 0;
}
