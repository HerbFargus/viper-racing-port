// world_wld_carobj.cpp -- group W5's rewrites (hook/wld_carobj.cpp: the CarObject, the view and the sky, the graf)
// against the originals, on random worlds, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_wld_carobj.cpp
//        /Fo<dir>\ /Fe<dir>\world_wld_carobj.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86 /STACK:16777216
//   run:   world_wld_carobj.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. For the
// rewrite's pass every rewritten function's entry is patched with a jump to its rewrite (and restored for the
// original's pass), so a rewrite calling another by address -- fixup_ptr's recursion, ViewBegin's
// build_sky_model -> add_sky_square, the CarObject constructor's load_car_models -- runs rewrites all the way down.
//
// A world is an arena -- four CarObjects (built by the ORIGINAL constructor, then stirred), a physics message, a
// PhobData and car-list entry, the view's GameOptions, a car-manager entry, a sky texture's pixels, smoke puffs
// with a stub vtable, a bump heap for MemAlloc -- plus the image's .data/.bss (the view, sky, graf and world
// statics set at random), and a graf buffer holding a real "track.grf" from the test install's tracks (read-only,
// at start-up) or a generated scene graph. Stubbed, logged (a jump to a logger): the gx and mr renderer calls,
// OptionsGet, the resource and string-table code (scripted: LOD and cockpit tables, ResourceExists answers,
// ResourceGet's graf), the game's CRT (sprintf / atof / atoi / strchr / strstr / stricmp: the host's), LogPanic /
// LogReport, Random, MemAlloc, the smoke / skid / shadow / reflection objects' constructors, SkidClient's
// SkidOn / SkidOff / Clear, CarMgrRegisterCar / CarMgrGetInfo, PhysicsCreate, TerrainBegin / End. Everything
// else runs as the game's code: WorldObject's constructor / UpdateMessage / Create, GraphObject, the World*
// getters, WorldAddGob, WorldGetCarTexture, mrCameraDistance, the matrix helpers. The game's CRT fatal-error paths
// (_amsg_exit, _NMSG_WRITE, crtMessageBoxA, _fptrap, exit) raise an exception the pass's guard catches -- never a
// message box -- and Windows' error boxes are off. A graf the caller won't relocate is relocated first (by the
// original), as unrelocated offsets taken as pointers write at random low addresses, this process's stack among them.
//
// Each world picks a function; the arena, .data/.bss, the graf buffer and the stubs' script state are saved, the
// original runs, the results are kept, everything restored, the rewrite runs; the arena, .data/.bss, the graf
// buffer, the return value and the stubs' logs are compared, and every byte the original changed must lie in the
// rewrite's footprint unless it's replay_only. Main thread: each world runs at a random precision (24 or 53 bits).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#include "../hook/port.h"
#undef PORT_FN_BUILDS
struct RwReg {
    uint32_t addr; void* fn; const char* name; RwReg* next;
    static RwReg*& head() { static RwReg* h; return h; }
    RwReg(uint32_t a, void* f, const char* n) : addr(a), fn(f), name(n), next(head()) { head() = this; }
};
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;                                                      \
    static RwReg VP_CAT(rwreg_, NEW)(V10, (void*)&NEW, NAME);

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { printf("footprint: object() not expected (%s)\n", what); add(obj, 4, what); }
void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

#include "../hook/wld_carobj.cpp"

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
static void putf(void* p, float f) { memcpy(p, &f, 4); }
static void puti(void* p, int32_t v) { memcpy(p, &v, 4); }

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

// the rewrites, patched in for the rewrite's pass only
struct Patch { uint32_t addr; void* fn; uint8_t orig[5]; const char* name; };
static Patch g_patch[128];
static int g_npatch;
static void collect_rewrites() {
    for (RwReg* r = RwReg::head(); r; r = r->next) {
        Patch& p = g_patch[g_npatch++];
        p.addr = r->addr; p.fn = r->fn; p.name = r->name;
        memcpy(p.orig, (void*)r->addr, 5);
    }
}
static void rewrites_on(bool on) {
    for (int i = 0; i < g_npatch; i++) {
        if (on) patch_jmp(g_patch[i].addr, g_patch[i].fn);
        else memcpy((void*)g_patch[i].addr, g_patch[i].orig, 5);
    }
}

// ---- the world: an arena, the image's .data/.bss, the graf buffer -------------------------------------------
enum { ARENA_BYTES = 0x10000, CAR_STRIDE = 0x900 };
enum : uint32_t {
    A_CARS = 0x0000,               // 4 CarObjects
    A_MSG = 0x2400,                // the message (0x190)
    A_PD = 0x2600,                 // PhobData / CarData (0x200)
    A_CLE = 0x2800,                // CarListEntry (0x100)
    A_OPTS = 0x2900,               // ViewBegin's GameOptions
    A_INFO = 0x2980,               // CarMgrInfo
    A_PIX = 0x2a00,                // the sky texture's pixels (0x200)
    A_VT = 0x2c00,                 // the smoke puffs' vtable (0x40)
    A_SMOKE = 0x2c40,              // 40 smoke puffs (8 bytes each)
    A_NAME = 0x2e00,               // GrafLoad's name
    A_P3 = 0x2f00,                 // GetParticlePosition's points
    A_HEAP = 0x3000, A_HEAP_END = 0x10000,
};
static uint8_t* g_arena;
static uint8_t g_arena_snap[ARENA_BYTES], g_arena_after[ARENA_BYTES];
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* g_data_snap;
static uint8_t* g_data_after;
static uint8_t* car(int i) { return g_arena + A_CARS + CAR_STRIDE * i; }
static CarObject* cobj(int i) { return (CarObject*)car(i); }

enum { GRAF_BYTES = 6 << 20 };
static uint8_t* g_graf;                         // the resource as loaded (fixed address)
static uint8_t* g_graf_snap;                    // its state before the passes
static uint8_t* g_graf_after;
static uint32_t g_graf_used;                    // bytes in play this world (compared)
struct GrafFile { std::string name; std::vector<uint8_t> data; };
static std::vector<GrafFile> g_grafs;

// ---- the stubs' log and script ------------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 16 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void log_word(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static void log_bytes(const void* p, int n) { for (int i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, (const uint8_t*)p + i, n - i < 4 ? n - i : 4); log_word(w); } }
static void log_str(const char* s) { if (!s) { log_word(0xdead0000); return; } size_t n = strlen(s); log_word((uint32_t)n); log_bytes(s, (int)n); }
static bool on_stack(const void* p) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    return (uintptr_t)p >= (uintptr_t)tib->StackLimit && (uintptr_t)p < (uintptr_t)tib->StackBase;
}
static uint32_t lp(const void* p) { return on_stack(p) ? 0x57ac0000u : (uint32_t)(uintptr_t)p; }   // a stack pointer differs per pass

struct Script {
    uint8_t exists[64];                 // ResourceExists, in call order
    int rand_vals[16];                  // Random
    uint8_t visible[64];                // the smoke puffs' IsVisible
    uint8_t alloc_null[16];             // MemAlloc, by allocation
    int video_mode; uint8_t vid_ok;
    float draw_dist, detail; uint8_t optb[10]; uint8_t opt_set[10];
    uint8_t mr_enabled[2];
    uint8_t grab_ok, format; uint16_t pixel;
    int stats[4];
    int lod_rows; uint8_t lod_null; char lod[12][3][24];
    char cockpit[6][4][24];
    float ext[3][6];
    uint8_t res_null; uint32_t ver; int32_t size; uint8_t first, reloc; void* res;
    int32_t phys_handle;
};
static Script g_script;
struct ScriptState { int handle, exists_i, rand_i, visible_i, alloc_i, ext_i; uint32_t heap; };
static ScriptState g_ss, g_ss_start;

static int next_handle() { return 0x1000 + g_ss.handle++; }

// the CRT
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    log_word('SPRF'); log_word(lp(buf)); log_word((uint32_t)fmt); log_str(buf);
    return r;
}
static double __cdecl stub_atof(const char* s) { log_word('ATOF'); log_str(s); return strtod(s, 0); }
static int __cdecl stub_atoi(const char* s) { log_word('ATOI'); log_str(s); return atoi(s); }
static const char* __cdecl stub_strchr(const char* s, int c) { log_word('SCHR'); log_str(s); log_word((uint32_t)c); return strchr(s, c); }
static const char* __cdecl stub_strstr(const char* s, const char* t) { log_word('SSTR'); log_str(s); log_str(t); return strstr(s, t); }
static int __cdecl stub_stricmp(const char* a, const char* b) { log_word('SCMP'); log_str(a); log_str(b); return _stricmp(a, b); }
static void log_fmt_args(const char* fmt, va_list ap) {
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == 's') log_str(va_arg(ap, const char*));
        else if (*p == 'd') log_word(va_arg(ap, uint32_t));
        else if (*p == 'f') { double d = va_arg(ap, double); log_bytes(&d, 8); }
    }
}
static void __cdecl stub_panic(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_word('PANC'); log_word((uint32_t)fmt); log_fmt_args(fmt, ap); va_end(ap); }
static void __cdecl stub_report(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_word('REPT'); log_word((uint32_t)fmt); log_fmt_args(fmt, ap); va_end(ap); }
static int __cdecl stub_random(int n) {
    log_word('RAND'); log_word((uint32_t)n);
    return g_script.rand_vals[g_ss.rand_i++ & 15];
}
// options: the key's value, unless it's "absent"
static int opt_slot(const char* key) {
    static const char* const keys[] = {"video_mode", "mipmap", "draw_distance", "detail_level", "sky_texture", "fog", "lighting", "filtering"};
    for (int i = 0; i < 8; i++) if (!strcmp(key, keys[i])) return i;
    return 9;
}
static void __cdecl stub_opt_int(const char* sect, const char* key, int* out) {
    log_word('OPTI'); log_str(sect); log_str(key); log_word(lp(out));
    if (g_script.opt_set[opt_slot(key)]) *out = g_script.video_mode;
}
static void __cdecl stub_opt_float(const char* sect, const char* key, float* out) {
    log_word('OPTF'); log_str(sect); log_str(key); log_word(lp(out));
    int s = opt_slot(key);
    if (g_script.opt_set[s]) *out = s == 2 ? g_script.draw_dist : g_script.detail;
}
static void __cdecl stub_opt_byte(const char* sect, const char* key, uint8_t* out) {
    log_word('OPTB'); log_str(sect); log_str(key); log_word(lp(out));
    int s = opt_slot(key);
    if (g_script.opt_set[s]) *out = g_script.optb[s];
}
// gx
static uint8_t __cdecl stub_vid_ok(int m) { log_word('VIDM'); log_word((uint32_t)m); return g_script.vid_ok; }
static void __cdecl stub_change_mode(int m) { log_word('CHMD'); log_word((uint32_t)m); }
static int __cdecl stub_get_texture(const char* name, int slot) { log_word('GTEX'); log_str(name); log_word((uint32_t)slot); return next_handle(); }
static uint8_t __cdecl stub_grab(int tex, uint8_t* canvas) {
    log_word('GRAB'); log_word((uint32_t)tex); log_word(lp(canvas));
    if (!g_script.grab_ok) return 0;
    canvas[0] = g_script.format;
    uint8_t* px = g_arena + A_PIX;
    memcpy(px + 0x102, &g_script.pixel, 2);
    memcpy(canvas + 4, &px, 4);
    return 1;
}
static void __cdecl stub_release() { log_word('RELT'); }
static void __cdecl stub_forget_tex(int t) { log_word('FGTX'); log_word((uint32_t)t); }
static void __cdecl stub_unload_tex(int t) { log_word('ULTX'); log_word((uint32_t)t); }
static void __cdecl stub_reload_tex(int t) { log_word('RLTX'); log_word((uint32_t)t); }
static void __cdecl stub_blit(int a, int b, uint32_t u0, uint32_t v0, uint32_t u1, uint32_t v1) {
    log_word('BLIT'); log_word((uint32_t)a); log_word((uint32_t)b); log_word(u0); log_word(v0); log_word(u1); log_word(v1);
}
static void __cdecl stub_alpha_blit(const char* name, int t) { log_word('ABLT'); log_str(name); log_word((uint32_t)t); }
static void* __cdecl stub_get_stamp(const char* name) { log_word('GSTP'); log_str(name); return (void*)(uintptr_t)(0x5a000000 + next_handle()); }
static void __cdecl stub_forget_stamp(void* s) { log_word('FSTP'); log_word((uint32_t)(uintptr_t)s); }
// mr
static void __cdecl stub_projection(uint32_t a, uint32_t b, uint32_t c) { log_word('PROJ'); log_word(a); log_word(b); log_word(c); }
static void __cdecl stub_background(uint32_t a, uint32_t b, uint32_t c) { log_word('BKGD'); log_word(a); log_word(b); log_word(c); }
static void __cdecl stub_begin_frame() { log_word('BFRM'); }
static void __cdecl stub_end_frame() { log_word('EFRM'); }
static void __cdecl stub_enable(int f) { log_word('ENAB'); log_word((uint32_t)f); }
static void __cdecl stub_disable(int f) { log_word('DISA'); log_word((uint32_t)f); }
static uint8_t __cdecl stub_is_enabled(int f) { log_word('ISEN'); log_word((uint32_t)f); return g_script.mr_enabled[f == 1 ? 0 : 1]; }
static void __cdecl stub_light_mode(int m) { log_word('LMOD'); log_word((uint32_t)m); }
static void __cdecl stub_mirror_view(uint8_t m) { log_word('MIRV'); log_word(m); }
static void __cdecl stub_stats(int* a, int* b, int* c, int* d) {
    log_word('STAT'); log_word(lp(a)); log_word(lp(b)); log_word(lp(c)); log_word(lp(d));
    *a = g_script.stats[0]; *b = g_script.stats[1]; *c = g_script.stats[2]; *d = g_script.stats[3];
}
static int __cdecl stub_model_create(const char* name, int n) { log_word('MCRE'); log_str(name); log_word((uint32_t)n); return next_handle(); }
static void __cdecl stub_build_lit(int m, const int32_t* info, int flags) { log_word('MBLT'); log_word((uint32_t)m); log_word(lp(info)); log_word((uint32_t)flags); log_bytes(info, 40); }
static void __cdecl stub_model_destroy(int m) { log_word('MDES'); log_word((uint32_t)m); }
static int __cdecl stub_model_load(const char* name) { log_word('MLOD'); log_str(name); return next_handle(); }
static int __cdecl stub_load_remap(const char* name, const void* remap, int f) {
    log_word('MLRM'); log_str(name); log_word(lp(remap)); log_word((uint32_t)f); log_bytes(remap, 0x28);
    return next_handle();
}
static int __cdecl stub_copy_remap(int m, const void* remap, int f) { log_word('MCRM'); log_word((uint32_t)m); log_word(lp(remap)); log_word((uint32_t)f); return next_handle(); }
static int __cdecl stub_model_copy(int m) { log_word('MCPY'); log_word((uint32_t)m); return next_handle(); }
static void __cdecl stub_model_unload(int m) { log_word('MUNL'); log_word((uint32_t)m); }
static void __cdecl stub_destroy_copy(int m) { log_word('MDCP'); log_word((uint32_t)m); }
static void __cdecl stub_extents(int m, float* a, float* b, float* c, float* d, float* e, float* f) {
    log_word('MEXT'); log_word((uint32_t)m);
    // the six pointers relative to the first (the original's are six consecutive stack floats)
    log_word((uint32_t)((uint8_t*)b - (uint8_t*)a)); log_word((uint32_t)((uint8_t*)c - (uint8_t*)a)); log_word((uint32_t)((uint8_t*)d - (uint8_t*)a));
    log_word((uint32_t)((uint8_t*)e - (uint8_t*)a)); log_word((uint32_t)((uint8_t*)f - (uint8_t*)a));
    const float* x = g_script.ext[g_ss.ext_i++ % 3];
    *a = x[0]; *b = x[1]; *c = x[2]; *d = x[3]; *e = x[4]; *f = x[5];
}
static int __cdecl stub_copy_transform(int m, const float* mat) { log_word('MCTR'); log_word((uint32_t)m); log_bytes(mat, 36); return next_handle(); }
// resources and string tables
static void* __cdecl stub_res_get(const char* name, uint32_t type, uint32_t* ver, int32_t* size, uint8_t* first, uint8_t* reloc) {
    log_word('RGET'); log_str(name); log_word(type); log_word(lp(ver)); log_word(lp(size)); log_word(lp(first)); log_word(lp(reloc));
    if (g_script.res_null) return 0;
    *ver = g_script.ver; *size = g_script.size; *first = g_script.first; *reloc = g_script.reloc;
    return g_script.res;
}
static uint8_t __cdecl stub_res_forget(void* p) { log_word('RFGT'); log_word((uint32_t)(uintptr_t)p); return 1; }
static uint8_t __cdecl stub_res_exists(const char* name) { log_word('REXS'); log_str(name); return g_script.exists[g_ss.exists_i++ & 63]; }
static void* __cdecl stub_st_get(const char* name) {
    log_word('STGT'); log_str(name);
    size_t n = strlen(name);
    if (n >= 5 && !strcmp(name + n - 5, "L.tab")) return g_script.lod_null ? 0 : (void*)0x7ab1e001;
    return (void*)0x7ab1e002;
}
static int __cdecl stub_st_rows(const void* st) { log_word('STNR'); log_word((uint32_t)(uintptr_t)st); return st == (void*)0x7ab1e001 ? g_script.lod_rows : st ? 6 : 0; }
static const char* __cdecl stub_st_entry(const void* st, int row, int col) {
    log_word('STEN'); log_word((uint32_t)(uintptr_t)st); log_word((uint32_t)row); log_word((uint32_t)col);
    if (st == (void*)0x7ab1e001 && row >= 0 && row < 12 && col >= 0 && col < 3) return g_script.lod[row][col];
    if (st == (void*)0x7ab1e002 && row >= 0 && row < 6 && col >= 0 && col < 4) return g_script.cockpit[row][col];
    return "";
}
static void __cdecl stub_st_forget(void* st) { log_word('STFG'); log_word((uint32_t)(uintptr_t)st); }
// the world and the objects
static void __cdecl stub_terrain_begin() { log_word('TBEG'); }
static void __cdecl stub_terrain_end() { log_word('TEND'); }
static void __cdecl stub_register(int i, const void* msg, const char* a, const char* b, int k) {
    log_word('CREG'); log_word((uint32_t)i); log_word(lp(msg)); log_str(a); log_str(b); log_word((uint32_t)k);
}
static const uint8_t* __cdecl stub_car_info(int i) { log_word('CINF'); log_word((uint32_t)i); return g_arena + A_INFO; }
static void __cdecl stub_phys_create(void* pd, int32_t* out) { log_word('PCRE'); log_word(lp(pd)); log_word(lp(out)); *out = g_script.phys_handle; }
static void* __cdecl stub_mem_alloc(int n) {
    log_word('MALC'); log_word((uint32_t)n);
    if (g_script.alloc_null[g_ss.alloc_i++ & 15]) return 0;
    uint32_t at = (g_ss.heap + 15) & ~15u;
    if (at + n + 4 > A_HEAP_END) { log_word('HEAP'); return 0; }
    g_ss.heap = at + n + 4;
    memset(g_arena + at, 0xa3, n + 4);
    return g_arena + at + 4;
}
static uint8_t __fastcall stub_smoke_visible(void* self, int) {
    log_word('SMVI'); log_word(lp(self));
    return g_script.visible[g_ss.visible_i++ & 63];
}
static void* __fastcall stub_smoke_ctor(void* self, int) { log_word('SMOK'); log_word(lp(self)); *(uint8_t**)self = g_arena + A_VT; return self; }
static void* __fastcall stub_skidobj_ctor(void* self, int, int n) { log_word('SKOB'); log_word(lp(self)); log_word((uint32_t)n); return self; }
static void* __fastcall stub_shadow_ctor(void* self, int, const char* name, void* o) { log_word('SHDW'); log_word(lp(self)); log_str(name); log_word(lp(o)); return self; }
static void* __fastcall stub_reflect_ctor(void* self, int, const char* name, void* o) { log_word('REFL'); log_word(lp(self)); log_str(name); log_word(lp(o)); return self; }
static void __fastcall stub_skid_on(void* self, int, const P3* at, const P3* up) { log_word('SKON'); log_word(lp(self)); log_bytes(at, 12); log_word(lp(up)); log_bytes(up, 12); }
static void __fastcall stub_skid_off(void* self, int) { log_word('SKOF'); log_word(lp(self)); }
static void __fastcall stub_skid_clear(void* self, int) { log_word('SKCL'); log_word(lp(self)); }

// the game's CRT fatal-error paths (its start-up never ran here): never a message box -- an exception the guard
// around each pass catches, counted as a fault of that pass
static void __cdecl stub_crt_fatal(int code) {
    log_word('CRTF'); log_word((uint32_t)code);
    RaiseException(0xe0c0f00d, 0, 0, 0);
}
static int g_world = -1;
static const char* g_phase = "start-up";
static LONG WINAPI unhandled(EXCEPTION_POINTERS* e) {
    printf("UNHANDLED %08lx at %08x (address %08x) in world %d, %s\n", e->ExceptionRecord->ExceptionCode, (uint32_t)e->ContextRecord->Eip,
           e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0, g_world, g_phase);
    fflush(stdout);
    ExitProcess(3);
}
static void install_stubs() {
    {
        const uint32_t fatal[] = {0x004cf730, 0x004d45b0, 0x004d6850, 0x004d5c10, 0x004d1c70, 0x004d1c90};   // _amsg_exit, _NMSG_WRITE,
        for (uint32_t a : fatal) patch_jmp(a, (void*)&stub_crt_fatal);                                         // crtMessageBoxA, _fptrap, exit, _exit
    }
    struct { uint32_t at; void* to; } s[] = {
        {0x004cf0a0, (void*)&stub_sprintf}, {0x004cfe40, (void*)&stub_atof}, {0x004cf990, (void*)&stub_atoi},
        {0x004ce0c0, (void*)&stub_strchr}, {0x004cf9a0, (void*)&stub_strstr}, {0x004da350, (void*)&stub_stricmp},
        {0x004112b0, (void*)&stub_panic}, {0x00411150, (void*)&stub_report}, {0x0041b6e0, (void*)&stub_random},
        {0x004713e0, (void*)&stub_opt_int}, {0x00471350, (void*)&stub_opt_float}, {0x00471470, (void*)&stub_opt_byte},
        {0x00454910, (void*)&stub_vid_ok}, {0x0044e040, (void*)&stub_change_mode}, {0x0044e240, (void*)&stub_get_texture},
        {0x0044e490, (void*)&stub_grab}, {0x0044e4d0, (void*)&stub_release}, {0x0044e280, (void*)&stub_forget_tex},
        {0x0044e450, (void*)&stub_unload_tex}, {0x0044e410, (void*)&stub_reload_tex}, {0x0044e380, (void*)&stub_blit},
        {0x0044e3d0, (void*)&stub_alpha_blit}, {0x00453500, (void*)&stub_get_stamp}, {0x004535d0, (void*)&stub_forget_stamp},
        {0x0044ede0, (void*)&stub_projection}, {0x0044ed60, (void*)&stub_background}, {0x0044e970, (void*)&stub_begin_frame},
        {0x0044e980, (void*)&stub_end_frame}, {0x00457890, (void*)&stub_enable}, {0x00457b00, (void*)&stub_disable},
        {0x00457d80, (void*)&stub_is_enabled}, {0x0045bf70, (void*)&stub_light_mode}, {0x0044ee20, (void*)&stub_mirror_view},
        {0x0044e820, (void*)&stub_stats}, {0x00455b40, (void*)&stub_model_create}, {0x00455ca0, (void*)&stub_build_lit},
        {0x00455bc0, (void*)&stub_model_destroy}, {0x004558c0, (void*)&stub_model_load}, {0x00455900, (void*)&stub_load_remap},
        {0x00455a10, (void*)&stub_copy_remap}, {0x00455b10, (void*)&stub_model_copy}, {0x00455950, (void*)&stub_model_unload},
        {0x00455b30, (void*)&stub_destroy_copy}, {0x00456ec0, (void*)&stub_extents}, {0x00455aa0, (void*)&stub_copy_transform},
        {0x00419fa0, (void*)&stub_res_get}, {0x0041a450, (void*)&stub_res_forget}, {0x00419d10, (void*)&stub_res_exists},
        {0x0041b210, (void*)&stub_st_get}, {0x0041b290, (void*)&stub_st_rows}, {0x0041b2a0, (void*)&stub_st_entry},
        {0x0041b270, (void*)&stub_st_forget}, {0x00465aa0, (void*)&stub_terrain_begin}, {0x00465ac0, (void*)&stub_terrain_end},
        {0x00464390, (void*)&stub_register}, {0x00464490, (void*)&stub_car_info}, {0x0042bc10, (void*)&stub_phys_create},
        {0x004140e0, (void*)&stub_mem_alloc}, {0x00467c80, (void*)&stub_smoke_ctor}, {0x004687d0, (void*)&stub_skidobj_ctor},
        {0x00467480, (void*)&stub_shadow_ctor}, {0x00468290, (void*)&stub_reflect_ctor}, {0x00468ae0, (void*)&stub_skid_on},
        {0x00468e20, (void*)&stub_skid_off}, {0x00468e40, (void*)&stub_skid_clear},
    };
    for (auto& x : s) patch_jmp(x.at, x.to);
}

// ---- building the world -----------------------------------------------------------------------------------------
static void rotation(float* m, bool upright) {
    double yaw = range(-3.1416f, 3.1416f);
    double lim = upright ? 0.35 : 3.1416;
    double pitch = range((float)-lim, (float)lim), roll = range((float)-lim, (float)lim);
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch), cr = cos(roll), sr = sin(roll);
    double R[9] = {cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr,
                   -cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr,
                   sy * cp, -sp, cy * cp};
    for (int i = 0; i < 9; i++) m[i] = (float)R[i];
}
static const char* const k_cars[] = {"viper", "gts", "a", "bmwm3", "vipergt", "p9"};
static const char* const k_drivers[] = {"Herb", "", "Jonathan Q", "x"};

// the view / world statics a function may read
static uint8_t* const OPTS = (uint8_t*)0x00554024;        // WorldGameOptions()
static void random_world_statics() {
    puti((void*)0x004f4358, chance(50) ? (int)(rnd() % 4) : (int)(rnd() % 16));    // focus car
    puti((void*)0x004f4354, (int)(rnd() % 16));                                     // player car
    puti((void*)0x0055335c, chance(90) ? (int)(rnd() % 13) : (int)rnd());          // camera type
    puti((void*)0x00522a1c, chance(85));                                            // the world's on
    puti((void*)0x00553358, chance(20) ? 0 : (int)(rnd() % 6));                     // skids
    puti((void*)0x0055336c, chance(30) ? 0 : chance(90) ? (int)(rnd() % 4) : (int)rnd());   // smoke
    puti((void*)0x005522c8, chance(50));                                            // shadow
    *(uint8_t*)0x0055404c = (uint8_t)chance(70);                                    // FX
    *(uint8_t*)0x00521608 = (uint8_t)chance(15);                                    // paused
    puti((void*)0x00553368, (int)(rnd() % 0x300));                                  // gob count
    puti(OPTS + 8, chance(50) ? 1 : (int)(rnd() % 3));                              // reflections
    puti(OPTS + 0x20, (int)(rnd() % 6));                                            // detail
    OPTS[0x24] = (uint8_t)chance(60);                                               // damage textures
    *(uint8_t*)0x004f4c9c = (uint8_t)chance(20);                                    // mirror mode
    puti((void*)0x00555870, chance(90) ? (int)(rnd() % 4) : (int)(rnd() % 11) - 5);  // min car LOD
    for (int i = 0; i < 3; i++) putf((void*)(0x00522a00 + 4 * i), range(-2000.0f, 2000.0f));   // the camera
    putf((void*)0x004e53bc, chance(70) ? 1.0f : range(0.2f, 3.0f));                  // gxLODFactor
    for (int i = 0; i < 16; i++) ((uint8_t**)0x00558068)[i] = chance(70) ? car(rnd() % 4) : 0;
}

static void random_script() {
    Script& s = g_script;
    for (int i = 0; i < 64; i++) s.exists[i] = (uint8_t)chance(60);
    for (int i = 0; i < 16; i++) s.rand_vals[i] = chance(95) ? (int)(rnd() % 1000) : (int)rnd();
    for (int i = 0; i < 64; i++) s.visible[i] = (uint8_t)chance(70);
    for (int i = 0; i < 16; i++) s.alloc_null[i] = (uint8_t)chance(4);
    s.video_mode = chance(50) ? 2 : (int)(rnd() % 8);
    s.vid_ok = (uint8_t)chance(80);
    s.draw_dist = chance(90) ? range(0.0f, 1.0f) : wildf();
    int dk = rnd() % 10;
    s.detail = dk < 7 ? range(0.0f, 1.0f) : dk < 8 ? bitsf(0x3f000000 + (rnd() % 3) - 1) : dk < 9 ? bitsf(0x3f400000 + (rnd() % 3) - 1) : wildf();
    for (int i = 0; i < 10; i++) s.optb[i] = (uint8_t)(chance(90) ? rnd() % 2 : rnd());
    for (int i = 0; i < 10; i++) s.opt_set[i] = (uint8_t)chance(85);
    s.mr_enabled[0] = (uint8_t)chance(50); s.mr_enabled[1] = (uint8_t)chance(50);
    s.grab_ok = (uint8_t)chance(85);
    s.format = (uint8_t)(chance(45) ? 4 : chance(80) ? 3 : rnd() % 8);
    s.pixel = (uint16_t)rnd();
    for (int i = 0; i < 4; i++) s.stats[i] = (int)rnd();
    // the LOD table: distance, faces ("12", "x", "3x"), "spec" / "alpha" / both / none
    int k = rnd() % 10;
    s.lod_rows = k < 1 ? 0 : k < 9 ? 1 + (int)(rnd() % 8) : 9 + (int)(rnd() % 2);
    s.lod_null = (uint8_t)chance(3);
    for (int r = 0; r < 12; r++) {
        sprintf(s.lod[r][0], "%.3f", chance(90) ? range(0.0f, 400.0f) : range(-1e6f, 1e6f));
        int f = rnd() % 10;
        if (f < 6) sprintf(s.lod[r][1], "%d", (int)(rnd() % 3000));
        else if (f < 8) strcpy(s.lod[r][1], "x");
        else sprintf(s.lod[r][1], "%dx", (int)(rnd() % 100));
        static const char* const fl[] = {"", "spec", "alpha", "spec alpha", "specalpha", "Alpha"};
        strcpy(s.lod[r][2], fl[rnd() % 6]);
    }
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < 4; c++) sprintf(s.cockpit[r][c], "%g", r >= 4 ? (c == 3 ? (chance(10) ? 0.0f : range(1.0f, 9000.0f)) : range(-200.0f, 200.0f)) : range(-2.0f, 2.0f));
    for (int j = 0; j < 3; j++) {
        float* e = s.ext[j];
        for (int i = 0; i < 3; i++) { e[2 * i] = range(-0.6f, 0.0f); e[2 * i + 1] = range(0.0f, 0.6f); }
        if (chance(5)) e[1] = e[0];                                      // a flat model: width / 0
        if (chance(3)) wild_at(&e[rnd() % 6]);
    }
    s.res_null = (uint8_t)chance(5);
    s.ver = chance(90) ? 3 : rnd() % 5;
    s.size = (int32_t)rnd();
    s.first = (uint8_t)chance(80);
    s.reloc = (uint8_t)chance(85);
    s.res = g_graf;
    s.phys_handle = (int32_t)rnd();
}

static void random_pd(uint8_t* pd, int index) {
    for (int k = 0; k < 0x200; k += 4) putf(pd + k, range(-100.0f, 100.0f));
    int t = rnd() % 10;
    puti(pd, t < 5 ? 0x41434152 : t < 9 ? 0x50434152 : (int)rnd());
    putf(pd + 0x188, chance(50) ? range(-1.5f, -0.4f) : chance(90) ? range(-1.5f, 1.5f) : wildf());
    puti(pd + 0x194, index);
    putf(pd + 0x130, range(150.0f, 350.0f)); putf(pd + 0x134, range(25.0f, 75.0f)); putf(pd + 0x138, range(13.0f, 20.0f));
    putf(pd + 0x13c, range(150.0f, 350.0f)); putf(pd + 0x140, range(25.0f, 75.0f)); putf(pd + 0x144, range(13.0f, 20.0f));
    if (chance(3)) wild_at(pd + 0x130 + 4 * (rnd() % 6));
}
static void random_cle(uint8_t* cle) {
    memset(cle, 0, 0x100);
    for (int k = 0; k < 0x100; k++) cle[k] = (uint8_t)rnd();
    puti(cle, (int)(rnd() % 4));
    strcpy((char*)cle + 4, k_drivers[rnd() % 4]);
    strcpy((char*)cle + 0x11, k_cars[rnd() % 6]);
    puti(cle + 0xc0, (int)(rnd() % 8) - 2);
}

// a CarObject by the ORIGINAL constructor (GhostCarObject's sometimes), with the rewrites off
typedef void*(__fastcall* CarCtorT)(void*, int, void*, const void*);
static int g_setup_faults;
static void build_car(int i, bool ghost) {
    const int rows = g_script.lod_rows;
    if (rows > 8) g_script.lod_rows = 8;                 // (more than 8 rows overrun the table: tested on their own)
    uint8_t* pd = g_arena + A_PD;
    random_pd(pd, chance(95) ? (int)(rnd() % 16) : 15);
    random_cle(g_arena + A_CLE);
    memset(car(i), 0xcd, CAR_STRIDE);
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    __try { ((CarCtorT)(ghost ? 0x0046cd20 : 0x00469e00))(car(i), 0, pd, g_arena + A_CLE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_setup_faults++; }
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    g_script.lod_rows = rows;
}

// a physics message, as Car::GetMessage writes it (a ghost's: + its car's index)
static void random_message(uint8_t* m, const CarObject* c) {
    for (int k = 0; k < 0x190; k += 4) putf(m + k, range(-100.0f, 100.0f));
    *(uint16_t*)m = (uint16_t)rnd();
    rotation((float*)(m + 4), !chance(15));
    for (int k = 0; k < 3; k++) putf(m + 0x28 + 4 * k, (c && chance(60) ? c->frame.pos.x : 0.0f) + range(-2000.0f, 2000.0f));
    putf(m + 0x158, chance(60) ? 1.0f : range(0.0f, 1.2f));
    for (int w = 0; w < 4; w++) {
        uint8_t* wm = m + 0x78 + 0x38 * w;
        putf(wm, (w & 1 ? 1.0f : -1.0f) * range(0.6f, 1.0f));
        putf(wm + 4, range(-0.6f, 0.1f));
        putf(wm + 8, (w < 2 ? 1.0f : -1.0f) * range(1.0f, 1.8f));
        putf(wm + 0x10, w < 2 ? range(-0.6f, 0.6f) : 0.0f);
        putf(wm + 0x14, range(-0.1f, 0.1f));
        wm[0x34] = (uint8_t)(chance(50) ? rnd() : rnd() & 0x41);
    }
    uint16_t* g = (uint16_t*)(m + 0x162);
    const uint16_t* old = c ? (const uint16_t*)(c->msg + 0x162) : 0;
    int gk = rnd() % 10;
    for (int i = 0; i < 16; i++) {
        uint16_t v = old && gk < 6 ? old[i] : 0;
        if (gk < 4) { if (chance(10)) v |= (uint16_t)(1u << (rnd() % 16)); }
        else if (gk < 6) { if (chance(10)) v ^= (uint16_t)(1u << (rnd() % 16)); }
        else if (gk < 8) v = (uint16_t)(chance(20) ? rnd() : 0);
        else v = (uint16_t)rnd();
        g[i] = v;
    }
    puti(m + 0x18c, chance(90) ? (int)(rnd() % 16) : (int)(rnd() % 4));
    if (chance(10)) {
        int n = 1 + rnd() % 3;
        for (int i = 0; i < n; i++) {
            static const uint32_t offs[] = {0x4, 0x8, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x78, 0x7c, 0x80, 0x88, 0x8c,
                                            0xb0, 0xb4, 0xb8, 0xc4, 0xe8, 0xec, 0xf0, 0xfc, 0x120, 0x124, 0x128, 0x134, 0x158};
            wild_at(m + offs[rnd() % (sizeof offs / sizeof *offs)]);
        }
    }
}

// stir a constructed CarObject: its state between messages
static void stir_car(CarObject* c) {
    c->last_view_mode = chance(60) ? 0 : chance(80) ? (int)(rnd() % 3) : (int)rnd();
    c->view_mode = (int)(rnd() % 3);
    rotation(c->frame.rot.m, !chance(15));
    c->frame.pos = P3{range(-2000.0f, 2000.0f), range(-50.0f, 300.0f), range(-2000.0f, 2000.0f)};
    uint16_t* g = (uint16_t*)(c->msg + 0x162);
    for (int i = 0; i < 16; i++) g[i] = (uint16_t)(chance(30) ? rnd() : 0);
    for (int w = 0; w < 4; w++) {
        c->wheel_frame[w].pos = P3{c->frame.pos.x + range(-3.0f, 3.0f), c->frame.pos.y + range(-1.0f, 1.0f), c->frame.pos.z + range(-3.0f, 3.0f)};
        if (chance(3)) wild_at(&c->wheel_frame[w].pos.x + rnd() % 3);
    }
    if (chance(5)) wild_at(chance(50) ? &c->radius_front : &c->radius_rear);
    // the LOD state
    if (chance(20)) c->num_lods = (int)(rnd() % 10) - 1;
    for (int r = 0; r < 8; r++) if (chance(30)) c->lod[r].dist = chance(90) ? range(0.0f, 500.0f) : wildf();
    for (int r = 0; r < 8; r++) c->lod[r].alpha = (uint8_t)(chance(50) ? rnd() % 2 : rnd());
    c->lod_now = chance(95) ? (int)(rnd() % 8) : (int)(rnd() % 12) - 2;
    if (c->lod_now < 0 || c->lod_now > 9) c->lod_now = 0;
    putf(c->msg + 0x158, chance(40) ? 1.0f : chance(70) ? range(-0.2f, 1.5f) : wildf());
    // smoke puffs (find_available_smoke)
    c->num_smoke = chance(90) ? (int)(rnd() % 33) : (int)(rnd() % 40) - 5;
    for (int i = 0; i < 32; i++) c->smoke[i] = g_arena + A_SMOKE + 8 * (rnd() % 40);
}

// ---- the functions under test --------------------------------------------------------------------------------------
enum Kind {
    K_PREBEGIN, K_POSTEND, K_BEGIN, K_END, K_ENTER, K_LEAVE, K_ISMIRROR, K_ISSKY, K_MINLOD, K_DYNOCLIP, K_STATICCLIP,
    K_SETSKY, K_RESETSKY, K_ADDSQ, K_ADDTOP, K_BUILDSKY, K_DESTROYSKY,
    K_GRAFLOAD, K_GRAFUNLOAD, K_LOOKUP, K_SETMIN, K_SETBIAS, K_SETCLIP, K_FIXGRAF, K_FIXPTR, K_FIXOBJS, K_CLEANUP,
    K_CTOR, K_GCTOR, K_DTOR, K_GDTOR, K_UPDATE, K_ISALPHA, K_ISVISIBLE, K_FINDSMOKE, K_UPDMSG, K_GUPDMSG,
    K_LOADMODELS, K_INITTEX, K_LOADWHEELS, K_RANDREAL, K_PARTICLE, K_TRIVIAL,
    N_KINDS
};
static const char* const kind_names[N_KINDS] = {
    "ViewPreBegin", "ViewPostEnd", "ViewBegin", "ViewEnd", "ViewEnterMirrorMode", "ViewLeaveMirrorMode", "ViewIsMirrorMode",
    "ViewIsSkyOn", "ViewGetMinCarLOD", "ViewGetDynoClipDist", "ViewGetStaticClipDist", "ViewSetSky", "reset_sky_square",
    "add_sky_square", "add_sky_top", "build_sky_model", "destroy_sky_model",
    "GrafLoad", "GrafUnload", "GrafLookupDynoModel", "GrafSetMinLOD", "GrafSetLODBias", "GrafSetClipDistance", "fixup_graf",
    "fixup_ptr", "fixup_objs", "cleanup_objs",
    "CarObject::CarObject", "GhostCarObject::GhostCarObject", "CarObject::~CarObject", "GhostCarObject::~GhostCarObject",
    "CarObject::Update", "CarObject::IsAlpha", "CarObject::IsVisible", "CarObject::find_available_smoke",
    "CarObject::UpdateMessage", "GhostCarObject::UpdateMessage", "CarObject::load_car_models", "CarObject::InitTextures",
    "CarObject::load_wheels", "RandomReal", "GetParticlePosition", "the trivial constructors / IsGhostCar"};
static const int kind_weight[N_KINDS] = {
    10, 5, 60, 20, 10, 10, 3, 3, 3, 5, 5, 5, 3, 40, 30, 40, 3,
    12, 4, 8, 5, 5, 5, 6, 8, 8, 5,
    60, 20, 20, 8, 120, 60, 10, 40, 300, 150, 40, 20, 30, 10, 20, 8};

struct Args { uint32_t u[8]; int i; int car; float f[2]; };
static Args g_args;

typedef void(__cdecl* V_t)();
typedef void(__cdecl* VP_t)(const void*);
typedef uint8_t(__cdecl* B_t)();
typedef int(__cdecl* I_t)();
typedef float(__cdecl* F_t)();
typedef void(__cdecl* VU2_t)(uint32_t, uint32_t);
typedef void(__cdecl* VU7_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int);
typedef void(__cdecl* VI_t)(int);
typedef int(__cdecl* IP_t)(void*);
typedef int(__cdecl* II_t)(int);
typedef void(__cdecl* VU_t)(uint32_t);
typedef void(__cdecl* VPPB_t)(void*, void*, uint8_t);
typedef void(__cdecl* VPP_t)(void*, void*);
typedef void(__cdecl* VPB_t)(void*, uint8_t);
typedef void*(__fastcall* TCtor_t)(void*, int, void*, const void*);
typedef void(__fastcall* TV_t)(void*, int);
typedef uint8_t(__fastcall* TB_t)(void*, int);
typedef int(__fastcall* TI_t)(void*, int);
typedef void(__fastcall* TP_t)(void*, int, const void*);
typedef void(__fastcall* TLoad_t)(void*, int, const char*, int, uint8_t);
typedef double(__cdecl* RR_t)(uint32_t, uint32_t);
typedef double(__cdecl* DR_t)();
typedef void(__cdecl* GPP_t)(void*, const void*, const void*, uint32_t);
typedef void*(__fastcall* TR_t)(void*, int);

// each call through the function's own address: the original's pass has the originals there, the rewrite's
// pass the jumps to the rewrites
static uint64_t call_kind(Kind k) {
    uint64_t r = 0;
    CarObject* c = cobj(g_args.car);
    switch (k) {
    case K_PREBEGIN: ((V_t)0x00465ff0)(); break;
    case K_POSTEND: ((V_t)0x004660b0)(); break;
    case K_BEGIN: ((VP_t)0x004660d0)(g_arena + A_OPTS); break;
    case K_END: ((V_t)0x004664e0)(); break;
    case K_ENTER: ((V_t)0x004666f0)(); break;
    case K_LEAVE: ((V_t)0x00466780)(); break;
    case K_ISMIRROR: r = ((B_t)0x004667f0)(); break;
    case K_ISSKY: r = ((B_t)0x00466800)(); break;
    case K_MINLOD: r = (uint32_t)((I_t)0x00466810)(); break;
    case K_DYNOCLIP: { double d = ((DR_t)0x00466820)(); memcpy(&r, &d, 8); break; }     // ST0 whole
    case K_STATICCLIP: { float f = ((F_t)0x00466830)(); r = fbits(f); break; }
    case K_SETSKY: ((VU2_t)0x00466870)(g_args.u[0], g_args.u[1]); break;
    case K_RESETSKY: ((V_t)0x00466890)(); break;
    case K_ADDSQ: ((VU7_t)0x004668b0)(g_args.u[0], g_args.u[1], g_args.u[2], g_args.u[3], g_args.u[4], g_args.u[5], g_args.i); break;
    case K_ADDTOP: ((VU2_t)0x00466ae0)(g_args.u[0], g_args.u[1]); break;
    case K_BUILDSKY: ((VI_t)0x00466ce0)(g_args.i); break;
    case K_DESTROYSKY: ((V_t)0x00466e70)(); break;
    case K_GRAFLOAD: r = (uint32_t)((IP_t)0x0046db30)(g_arena + A_NAME); break;
    case K_GRAFUNLOAD: ((VI_t)0x0046dc20)(g_args.i); break;
    case K_LOOKUP: r = (uint32_t)((II_t)0x0046dc50)(g_args.i); break;
    case K_SETMIN: ((VU_t)0x0046dfb0)(g_args.u[0]); break;
    case K_SETBIAS: ((VU_t)0x0046dfc0)(g_args.u[0]); break;
    case K_SETCLIP: ((VU_t)0x0046dfd0)(g_args.u[0]); break;
    case K_FIXGRAF: ((VPPB_t)0x0046e130)(g_graf, g_graf, (uint8_t)g_args.i); break;
    case K_FIXPTR: ((VPP_t)0x0046e160)(g_graf, g_graf); break;
    case K_FIXOBJS: ((VPB_t)0x0046e1a0)(g_graf, (uint8_t)g_args.i); break;
    case K_CLEANUP: ((VP_t)0x0046e300)(g_graf); break;
    case K_CTOR: r = (uint32_t)(uintptr_t)((TCtor_t)0x00469e00)(c, 0, g_arena + A_PD, g_arena + A_CLE); break;
    case K_GCTOR: r = (uint32_t)(uintptr_t)((TCtor_t)0x0046cd20)(c, 0, g_arena + A_PD, g_arena + A_CLE); break;
    case K_DTOR: ((TV_t)0x0046a170)(c, 0); break;
    case K_GDTOR: ((TV_t)0x0046cd40)(c, 0); break;
    case K_UPDATE: ((TV_t)0x0046a390)(c, 0); break;
    case K_ISALPHA: r = ((TB_t)0x0046a470)(c, 0); break;
    case K_ISVISIBLE: r = ((TB_t)0x0046c2f0)(c, 0); break;
    case K_FINDSMOKE: r = (uint32_t)((TI_t)0x0046bca0)(c, 0); break;
    case K_UPDMSG: ((TP_t)0x0046bce0)(c, 0, g_arena + A_MSG); break;
    case K_GUPDMSG: ((TP_t)0x0046cd50)(c, 0, g_arena + A_MSG); break;
    case K_LOADMODELS: ((TLoad_t)0x0046c300)(c, 0, (const char*)g_arena + A_CLE + 0x11, g_args.i, (uint8_t)g_args.u[0]); break;
    case K_INITTEX: ((TV_t)0x0046c9d0)(c, 0); break;
    case K_LOADWHEELS: ((TP_t)0x0046ca60)(c, 0, g_arena + A_PD); break;
    case K_RANDREAL: { double d = ((RR_t)0x0046b450)(g_args.u[0], g_args.u[1]); memcpy(&r, &d, 8); break; }
    case K_PARTICLE: ((GPP_t)0x0046ad30)(g_arena + A_P3, g_arena + A_P3 + 16, g_arena + A_P3 + 32, g_args.u[0]); break;
    case K_TRIVIAL: {
        static const uint32_t at[] = {0x0046cd90, 0x0046cda0, 0x0046cde0, 0x0046cdf0};
        uint32_t a = at[g_args.i & 3];
        r = (uint32_t)(uintptr_t)((TR_t)a)(g_arena + A_P3, 0);
        r = r << 16 ^ ((TB_t)0x0046cdb0)(c, 0) << 8 ^ ((TB_t)0x0046ce20)(c, 0);
        break;
    }
    default: break;
    }
    if (k == K_ISMIRROR || k == K_ISSKY || k == K_ISALPHA || k == K_ISVISIBLE) r &= 0xff;
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
static int run_guarded(Kind k, bool rw, uint64_t* ret) {
    g_log.n = 0;
    g_fault_code = g_fault_eip = g_fault_addr = 0;
    g_ss = g_ss_start;
    rewrites_on(rw);
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, g_pc, _MCW_PC);
    int fault = 0;
    __try { *ret = call_kind(k); } __except (fault_filter(GetExceptionInformation())) { fault = 1; }
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    rewrites_on(false);
    return fault;
}

static void footprint_of(Kind k, Footprint& fp) {
    fp.n = 0; fp.pure = false; fp.replay_only = 0;
    CarObject* c = cobj(g_args.car);
    switch (k) {
    case K_PREBEGIN: fpof_ViewPreBegin(fp); break;
    case K_POSTEND: fpof_ViewPostEnd(fp); break;
    case K_BEGIN: fpof_ViewBegin(fp, g_arena + A_OPTS); break;
    case K_END: fpof_ViewEnd(fp); break;
    case K_ENTER: fpof_ViewEnterMirrorMode(fp); break;
    case K_LEAVE: fpof_ViewLeaveMirrorMode(fp); break;
    case K_ISMIRROR: fpof_ViewIsMirrorMode(fp); break;
    case K_ISSKY: fpof_ViewIsSkyOn(fp); break;
    case K_MINLOD: fpof_ViewGetMinCarLOD(fp); break;
    case K_DYNOCLIP: fpof_ViewGetDynoClipDist(fp); break;
    case K_STATICCLIP: fpof_ViewGetStaticClipDist(fp); break;
    case K_SETSKY: fpof_ViewSetSky(fp, g_args.u[0], g_args.u[1]); break;
    case K_RESETSKY: fpof_reset_sky_square(fp); break;
    case K_ADDSQ: fpof_add_sky_square(fp, g_args.u[0], g_args.u[1], g_args.u[2], g_args.u[3], g_args.u[4], g_args.u[5], g_args.i); break;
    case K_ADDTOP: fpof_add_sky_top(fp, g_args.u[0], g_args.u[1]); break;
    case K_BUILDSKY: fpof_build_sky_model(fp, g_args.i); break;
    case K_DESTROYSKY: fpof_destroy_sky_model(fp); break;
    case K_GRAFLOAD: fpof_GrafLoad(fp, (char*)g_arena + A_NAME); break;
    case K_GRAFUNLOAD: fpof_GrafUnload(fp, g_args.i); break;
    case K_LOOKUP: fpof_GrafLookupDynoModel(fp, g_args.i); break;
    case K_SETMIN: fpof_GrafSetMinLOD(fp, bitsf(g_args.u[0])); break;
    case K_SETBIAS: fpof_GrafSetLODBias(fp, bitsf(g_args.u[0])); break;
    case K_SETCLIP: fpof_GrafSetClipDistance(fp, g_args.u[0]); break;
    case K_FIXGRAF: fpof_fixup_graf(fp, (GrafNode*)g_graf, g_graf, (uint8_t)g_args.i); break;
    case K_FIXPTR: fpof_fixup_ptr(fp, (GrafNode*)g_graf, g_graf); break;
    case K_FIXOBJS: fpof_fixup_objs(fp, (GrafNode*)g_graf, (uint8_t)g_args.i); break;
    case K_CLEANUP: fpof_cleanup_objs(fp, (GrafNode*)g_graf); break;
    case K_CTOR: fpof_CarObject_ctor(fp, c, 0, g_arena + A_PD, g_arena + A_CLE); break;
    case K_GCTOR: fpof_GhostCarObject_ctor(fp, c, 0, g_arena + A_PD, g_arena + A_CLE); break;
    case K_DTOR: fpof_CarObject_dtor(fp, c, 0); break;
    case K_GDTOR: fpof_GhostCarObject_dtor(fp, c, 0); break;
    case K_UPDATE: fpof_CarObject_Update(fp, c, 0); break;
    case K_ISALPHA: fpof_CarObject_IsAlpha(fp, c, 0); break;
    case K_ISVISIBLE: fpof_CarObject_IsVisible(fp, c, 0); break;
    case K_FINDSMOKE: fpof_CarObject_find_available_smoke(fp, c, 0); break;
    case K_UPDMSG: fpof_CarObject_UpdateMessage(fp, c, 0, g_arena + A_MSG); break;
    case K_GUPDMSG: fpof_GhostCarObject_UpdateMessage(fp, c, 0, g_arena + A_MSG); break;
    case K_LOADMODELS: fpof_CarObject_load_car_models(fp, c, 0, (const char*)g_arena + A_CLE + 0x11, g_args.i, (uint8_t)g_args.u[0]); break;
    case K_INITTEX: fpof_CarObject_InitTextures(fp, c, 0); break;
    case K_LOADWHEELS: fpof_CarObject_load_wheels(fp, c, 0, g_arena + A_PD); break;
    case K_RANDREAL: fpof_RandomReal(fp, bitsf(g_args.u[0]), bitsf(g_args.u[1])); break;
    case K_PARTICLE: fpof_GetParticlePosition(fp, (P3*)(g_arena + A_P3), (P3*)(g_arena + A_P3 + 16), (P3*)(g_arena + A_P3 + 32), bitsf(g_args.u[0])); break;
    case K_TRIVIAL: fp.add(g_arena + A_P3, 0x2c, "trivial ctor"); break;
    default: break;
    }
}
static bool in_footprint(const Footprint& fp, const uint8_t* p) {
    for (int i = 0; i < fp.n; i++)
        if (p >= (const uint8_t*)fp.r[i].p && p < (const uint8_t*)fp.r[i].p + fp.r[i].n) return true;
    return false;
}

// ---- the graf: real ones from the test install, or generated -----------------------------------------------------
static void load_grafs(const char* dir) {
    char pat[MAX_PATH];
    const char* exts[] = {"*.trk", "*.tra"};
    for (const char* e : exts) {
        sprintf(pat, "%s\\%s", dir, e);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            char path[MAX_PATH];
            sprintf(path, "%s\\%s", dir, fd.cFileName);
            FILE* f = fopen(path, "rb");
            if (!f) continue;
            std::vector<uint8_t> d;
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            d.resize(n); fread(d.data(), 1, n, f); fclose(f);
            if (n < 16 || memcmp(d.data(), "0TSR", 4)) continue;
            int32_t count; memcpy(&count, &d[4], 4);
            size_t off = 16 + 36 * (size_t)count;
            for (int i = 0; i < count && off <= (size_t)n; i++) {
                char name[17] = {0}; uint8_t tag[4]; int32_t size;
                memcpy(name, &d[16 + 36 * i], 16); memcpy(tag, &d[16 + 36 * i + 16], 4); memcpy(&size, &d[16 + 36 * i + 24], 4);
                if (size < 0 || off + 8 + size > (size_t)n) break;
                if (!memcmp(tag, "FARG", 4) && size + 64 < GRAF_BYTES) {
                    bool dup = false;
                    for (auto& g : g_grafs) if (g.data.size() == (size_t)size && !memcmp(g.data.data(), &d[off + 8], size)) dup = true;
                    if (!dup) g_grafs.push_back({std::string(fd.cFileName) + "/" + name, std::vector<uint8_t>(d.begin() + off + 8, d.begin() + off + 8 + size)});
                }
                off += 8 + size;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}
// a generated scene graph: groups, sphere groups, LODs, models and facing models, links as resource offsets
static uint32_t g_gen_at;
static uint32_t gen_node(int depth) {
    int t = depth > 5 ? 3 + rnd() % 2 : (int)(rnd() % 5);
    uint32_t at = g_gen_at;
    uint8_t* n = g_graf + at;
    uint32_t size = 0x38;
    int32_t info[10] = {0};
    if (t == 3 || t == 4) {
        info[0] = chance(90) ? (int)(rnd() % 12) : -(int)(rnd() % 3);
        info[2] = chance(85) ? 1 + (int)(rnd() % 3) : 0;
        info[4] = (int)(rnd() % 10);
        info[6] = chance(20) ? (int)(rnd() % 3) : 0;
        info[8] = chance(20) ? (int)(rnd() % 3) : 0;
        size = (t == 3 ? 0x38 : 0x4c) + 32 * (info[0] > 0 ? info[0] : 0) + 32 * (info[2] > 0 ? info[2] : 0) + 8 * (info[4] > 0 ? info[4] : 0) + 16 * (info[6] > 0 ? info[6] : 0) + 8;
    }
    if (g_gen_at + size + 0x100 > GRAF_BYTES) return 0;
    for (uint32_t i = 0; i < size; i++) n[i] = (uint8_t)rnd();
    g_gen_at += (size + 3) & ~3u;
    puti(n, t);
    puti(n + 4, 0); puti(n + 8, 0);
    if (t == 2) {
        putf(n + 0x18, chance(90) ? range(0.0f, 500.0f) : wildf());
        putf(n + 0x1c, chance(40) ? -1.0f : chance(90) ? range(0.0f, 900.0f) : wildf());
    }
    if (t == 1) n[0xc] = (uint8_t)chance(50);
    if (t == 3 || t == 4) {
        memcpy(n + 0x10, info, 40);
        puti(n + 0xc, (int)rnd());
        if (t == 4) {
            puti(n + 0x38, chance(70) ? 4 : (int)(rnd() % 6));
            puti(n + 0x48, chance(90) ? (int)(rnd() % 24) : chance(50) ? -1 - (int)(rnd() % 5) : 512 + (int)(rnd() % 5));
        }
    }
    if (depth < 7 && t < 3 && chance(80)) puti(n + 4, (int32_t)gen_node(depth + 1));            // a child (via +4)
    if (depth < 7 && chance(55)) puti(n + 8, (int32_t)gen_node(depth + 1));                      // a sibling (via +8)
    return at;
}
static int g_graf_pick;                         // -1: generated
static void fresh_graf() {
    if (!g_grafs.empty() && chance(60)) {
        g_graf_pick = (int)(rnd() % g_grafs.size());
        const std::vector<uint8_t>& d = g_grafs[g_graf_pick].data;
        memcpy(g_graf, d.data(), d.size());
        g_graf_used = (uint32_t)d.size();
    } else {
        g_graf_pick = -1;
        g_gen_at = 0x10;                        // a group at 0, its first child at 0x10
        memset(g_graf, 0, 0x10);
        puti(g_graf, 0);
        g_gen_at = 0x40;
        uint32_t c = gen_node(0);
        puti(g_graf + 8, (int32_t)c);           // offset 0 can't be a link (0 means none)
        g_graf_used = g_gen_at;
    }
}

// the graf, for a kind: the state it needs, prepared by the ORIGINAL code
static void prepare_graf(Kind k) {
    fresh_graf();
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    __try {
        // a graf the caller won't relocate is one already relocated (unrelocated offsets as pointers would write at
        // random low addresses -- this process's stack among them)
        const bool pre = (k == K_GRAFLOAD && g_script.first && !g_script.reloc) || (k == K_FIXGRAF && !(uint8_t)g_args.i);
        if (pre || k == K_FIXOBJS || k == K_CLEANUP || k == K_GRAFUNLOAD) ((VPP_t)0x0046e160)(g_graf, g_graf);
        if (k == K_CLEANUP || k == K_GRAFUNLOAD) ((VPB_t)0x0046e1a0)(g_graf, (uint8_t)chance(50));
        if (k == K_GRAFUNLOAD) { puti((void*)0x0055997c, (int32_t)(uintptr_t)g_graf); puti((void*)0x00559978, (int32_t)(uintptr_t)(g_arena + A_NAME)); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { printf("  (setup: the graf's preparation faulted)\n"); }
    _controlfp_s(&cw, _PC_53, _MCW_PC);
}

static const char* where(uint32_t a, char* buf) {
    uint32_t base = (uint32_t)(uintptr_t)g_arena;
    if (a >= base && a < base + ARENA_BYTES) {
        uint32_t o = a - base;
        if (o < 4 * CAR_STRIDE) sprintf(buf, "car%u+0x%x", o / CAR_STRIDE, o % CAR_STRIDE);
        else sprintf(buf, "arena+0x%x", o);
        return buf;
    }
    uint32_t gb = (uint32_t)(uintptr_t)g_graf;
    if (a >= gb && a < gb + GRAF_BYTES) { sprintf(buf, "graf+0x%x", a - gb); return buf; }
    sprintf(buf, "0x%08x", a);
    return buf;
}

// ---- main ----------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    SetUnhandledExceptionFilter(unhandled);
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 50000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_wld_carobj.cpp");
    if (!s) { printf("run from a build of the repo's test\\world_wld_carobj.cpp\n"); return 2; }
    *s = 0;
    char install[MAX_PATH];
    sprintf(install, "%s\\..\\game-files\\installs\\v1.0-RC", exe);
    strcat(exe, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    install_stubs();
    collect_rewrites();
    // each followed by reserved, uncommitted memory: a faithful overrun (a LOD table of more than 8 rows moves the
    // rows' pointer and writes on) faults there instead of corrupting this process's heap
    g_arena = (uint8_t*)VirtualAlloc(VirtualAlloc(0, 2 * ARENA_BYTES, MEM_RESERVE, PAGE_NOACCESS), ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_graf = (uint8_t*)VirtualAlloc(VirtualAlloc(0, GRAF_BYTES + 0x100000, MEM_RESERVE, PAGE_NOACCESS), GRAF_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_graf_snap = (uint8_t*)malloc(GRAF_BYTES);
    g_graf_after = (uint8_t*)malloc(GRAF_BYTES);
    g_data_snap = (uint8_t*)malloc(DATA_BYTES);
    g_data_after = (uint8_t*)malloc(DATA_BYTES);
    load_grafs(install);
    printf("%d rewrites; %d distinct track.grf resources from %s\n", g_npatch, (int)g_grafs.size(), install);

    static Footprint fp;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    int differ = 0, faults = 0, fp_bad = 0, replay_only = 0, wild_runs = 0;
    int per[N_KINDS] = {0}, per_bad[N_KINDS] = {0}, per_fault[N_KINDS] = {0};
    // coverage
    int c_mode[3] = {0}, c_whole = 0, c_cells = 0, c_unload = 0, c_skid_on = 0, c_skid_off = 0, c_skid_clear = 0, c_upd_fp = 0;
    int c_smoke = 0, c_skids1 = 0, c_skids4 = 0, c_player = 0, c_shadow = 0, c_reflect = 0, c_high = 0, c_cockpit = 0, c_lodx = 0;
    int c_fmt[3] = {0}, c_grabfail = 0, c_tod_panic = 0, c_night = 0, c_fps = 0, c_real_graf = 0, c_gen_graf = 0, c_badver = 0;
    int c_crt = 0, c_dup = 0, c_wrongid = 0, c_graf_models = 0, c_lod_found = 0, c_ghost_clamp = 0, c_alpha[2] = {0}, c_smoke_none = 0;
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weight[i];
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), kk = 0;
        while (pick >= kind_weight[kk]) pick -= kind_weight[kk++];
        const Kind kind = (Kind)kk;
        g_world = it; g_phase = "set-up";
        memset(g_arena, 0, ARENA_BYTES);
        for (int i = 0; i < 40; i++) *(uint8_t**)(g_arena + A_SMOKE + 8 * i) = g_arena + A_VT;
        *(void**)(g_arena + A_VT + 0xc) = (void*)&stub_smoke_visible;
        strcpy((char*)g_arena + A_NAME, chance(80) ? "track.grf" : "other.grf");
        strcpy((char*)g_arena + A_INFO + 0x12, k_cars[rnd() % 6]);
        for (int i = 0; i < 0x200; i += 2) *(uint16_t*)(g_arena + A_PIX + i) = (uint16_t)rnd();
        for (int i = 0; i < 0x40; i += 4) puti(g_arena + A_OPTS + i, (int)(rnd() % 4));
        puti(g_arena + A_OPTS + 4, chance(90) ? (int)(rnd() % 3) : (int)rnd() % 7 - 3);
        random_script();
        random_world_statics();
        g_ss = ScriptState{0, 0, 0, 0, 0, 0, A_HEAP};
        g_graf_used = 0;
        // the view statics
        for (uint32_t a = 0x00555860; a < 0x005558b8; a += 4) putf((void*)a, range(0.0f, 2.0f));
        for (uint32_t a = 0x00555874; a < 0x0055587a; a++) *(uint8_t*)a = (uint8_t)(chance(90) ? rnd() % 2 : rnd());
        puti((void*)0x00555860, chance(50) ? 2 : (int)(rnd() % 6));
        putf((void*)0x00555864, chance(90) ? range(300.0f, 2000.0f) : wildf());
        putf((void*)0x00555880, range(-50.0f, 50.0f)); putf((void*)0x00555888, range(50.0f, 400.0f));
        puti((void*)0x005558a8, (int)(rnd() % 400000)); puti((void*)0x005571f0, chance(50) ? 0 : (int)(rnd() % 200000));
        puti((void*)0x004f4c88, (int)(rnd() % 50000));
        puti((void*)0x004f4ca0, (int)(rnd() % 190)); puti((void*)0x004f4ca4, (int)(rnd() % 90)); puti((void*)0x004f4ca8, (int)(rnd() % 45));
        for (int i = 0; i < 512; i++) puti((void*)(0x00559168 + 4 * i), chance(80) ? -1 : (int)(rnd() % 100));
        // the cars
        bool car_kind = kind >= K_CTOR && kind <= K_LOADWHEELS;
        g_args.car = (int)(rnd() % 4);
        if (car_kind) {
            for (int i = 0; i < 4; i++) {
                if (kind == K_CTOR || kind == K_GCTOR) { if (i == g_args.car) continue; }
                if (i != g_args.car && chance(50)) continue;
                build_car(i, (kind == K_GUPDMSG || kind == K_GDTOR) && i == g_args.car ? true : chance(20));
                if (kind != K_DTOR && kind != K_GDTOR && kind != K_LOADMODELS && kind != K_LOADWHEELS) stir_car(cobj(i));
            }
            if (kind == K_UPDMSG || kind == K_GUPDMSG) {
                // a message applied once already, half the time (the wheels' last positions)
                if (chance(50)) {
                    random_message(g_arena + A_MSG, cobj(g_args.car));
                    unsigned cw;
                    _controlfp_s(&cw, _PC_24, _MCW_PC);
                    __try { ((TP_t)0x0046bce0)(cobj(g_args.car), 0, g_arena + A_MSG); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    _controlfp_s(&cw, _PC_53, _MCW_PC);
                    stir_car(cobj(g_args.car));
                }
                random_message(g_arena + A_MSG, cobj(g_args.car));
                if (chance(50)) puti((void*)0x004f4358, cobj(g_args.car)->index);
            }
            if (kind == K_CTOR || kind == K_GCTOR || kind == K_LOADWHEELS) {
                random_pd(g_arena + A_PD, chance(95) ? (int)(rnd() % 16) : 15);
                random_cle(g_arena + A_CLE);
                if (chance(30)) puti((void*)0x004f4354, *(int32_t*)(g_arena + A_PD + 0x194));   // the player's car
            }
            if (kind == K_UPDATE && chance(70)) {               // the camera near the car: the LOD rows' range
                const P3 p = cobj(g_args.car)->frame.pos;
                const float d = range(0.0f, 600.0f);
                putf((void*)0x00522a00, p.x + d * 0.6f); putf((void*)0x00522a04, p.y + range(-5.0f, 5.0f)); putf((void*)0x00522a08, p.z - d * 0.8f);
            }
            if (kind == K_LOADMODELS) { random_cle(g_arena + A_CLE); g_args.i = (int)(rnd() % 8) - 2; g_args.u[0] = (uint32_t)chance(50); }
            if (kind == K_UPDATE && chance(30)) (void)0;
        }
        // the graf
        bool graf_kind = kind == K_GRAFLOAD || kind == K_GRAFUNLOAD || (kind >= K_FIXGRAF && kind <= K_CLEANUP);
        if (graf_kind) {
            if (kind == K_FIXGRAF || kind == K_FIXOBJS) g_args.i = chance(50) ? 1 : chance(80) ? 0 : (int)(rnd() & 0xff);
            prepare_graf(kind);
            (g_graf_pick >= 0 ? c_real_graf : c_gen_graf)++;
        }
        // the arguments
        switch (kind) {
        case K_SETSKY: case K_ADDTOP: g_args.u[0] = fbits(chance(90) ? range(-800.0f, 800.0f) : wildf()); g_args.u[1] = fbits(chance(90) ? range(-800.0f, 800.0f) : wildf()); break;
        case K_ADDSQ:
            for (int i = 0; i < 6; i++) g_args.u[i] = fbits(chance(95) ? range(-800.0f, 800.0f) : wildf());
            g_args.i = chance(80) ? (int)(rnd() % 32) : (int)rnd();
            break;
        case K_BUILDSKY: g_args.i = chance(60) ? 16 : 1 + (int)(rnd() % 32); break;
        case K_GRAFUNLOAD: g_args.i = chance(80) ? 1 : (int)(rnd() % 4); break;
        case K_LOOKUP: g_args.i = chance(80) ? (int)(rnd() % 512) : (int)rnd() % 2000 - 700; break;
        case K_SETMIN: case K_SETBIAS: case K_SETCLIP: g_args.u[0] = fbits(chance(85) ? range(0.0f, 2000.0f) : wildf()); break;
        case K_RANDREAL: g_args.u[0] = fbits(chance(85) ? range(-10.0f, 10.0f) : wildf()); g_args.u[1] = fbits(chance(85) ? range(-10.0f, 10.0f) : wildf()); break;
        case K_PARTICLE:
            for (int i = 0; i < 12; i++) putf(g_arena + A_P3 + 4 * i, chance(90) ? range(-100.0f, 100.0f) : wildf());
            g_args.u[0] = fbits(chance(90) ? range(0.0f, 3.0f) : wildf());
            break;
        case K_TRIVIAL: g_args.i = (int)(rnd() % 4); for (int i = 0; i < 0x30; i++) g_arena[A_P3 + i] = (uint8_t)rnd(); break;
        default: break;
        }
        g_pc = chance(50) ? _PC_53 : _PC_24;
        if (trace) printf("world %d: %s\n", it, kind_names[kind]);
        g_phase = "footprint";
        footprint_of(kind, fp);
        if (fp.replay_only) replay_only++;
        // run both
        g_ss_start = g_ss;
        memcpy(g_arena_snap, g_arena, ARENA_BYTES);
        memcpy(g_data_snap, DATA, DATA_BYTES);
        if (g_graf_used) memcpy(g_graf_snap, g_graf, g_graf_used);
        uint64_t ro = 0, rn = 0;
        g_phase = "the original's pass";
        int fo = run_guarded(kind, false, &ro);
        uint32_t fo_code = g_fault_code, fo_eip = g_fault_eip, fo_addr = g_fault_addr;
        g_fault_code = g_fault_eip = g_fault_addr = 0;
        memcpy(g_arena_after, g_arena, ARENA_BYTES);
        memcpy(g_data_after, DATA, DATA_BYTES);
        if (g_graf_used) memcpy(g_graf_after, g_graf, g_graf_used);
        g_log_orig = g_log;
        memcpy(g_arena, g_arena_snap, ARENA_BYTES);
        memcpy(DATA, g_data_snap, DATA_BYTES);
        if (g_graf_used) memcpy(g_graf, g_graf_snap, g_graf_used);
        g_phase = "the rewrite's pass";
        int fn = run_guarded(kind, true, &rn);
        g_phase = "comparing";
        per[kind]++;
        bool same = true;
        if (fo || fn) {
            faults++;
            per_fault[kind]++;
            // both must fault, on the same address, with the same state behind them (compared below)
            if (fo != fn || fo_code != g_fault_code || fo_addr != g_fault_addr) {
                printf("  MISMATCH world %d (%s): %s (%08x at %08x, address %08x) / %s (%08x at %08x, address %08x)\n", it, kind_names[kind],
                       fo ? "original faulted" : "original ran", fo_code, fo_eip, fo_addr, fn ? "rewrite faulted" : "rewrite ran",
                       g_fault_code, g_fault_eip, g_fault_addr);
                same = false;
            }
        }
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
                printf("    %s original %08x (%.9g) rewrite %08x (%.9g)\n", where(0x4e1000 + i, buf), x, bitsf(x), y, bitsf(y));
                same = false;
                break;
            }
        for (uint32_t i = 0; i < g_graf_used; i += 4)
            if (memcmp(g_graf_after + i, g_graf + i, 4)) {
                uint32_t x, y; memcpy(&x, g_graf_after + i, 4); memcpy(&y, g_graf + i, 4);
                if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
                printf("    graf+0x%x original %08x rewrite %08x\n", i, x, y);
                same = false;
                break;
            }
        if (ro != rn) {
            if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
            printf("    return: original %016llx rewrite %016llx\n", ro, rn);
            same = false;
        }
        if (g_log.n != g_log_orig.n || memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX))) {
            if (same) printf("  MISMATCH world %d (%s)\n", it, kind_names[kind]);
            printf("    the stub logs differ: %u / %u words\n", g_log_orig.n, g_log.n);
            uint32_t m = g_log.n < g_log_orig.n ? g_log.n : g_log_orig.n;
            if (m > LOG_MAX) m = LOG_MAX;
            for (uint32_t i = 0; i < m; i++) if (g_log.w[i] != g_log_orig.w[i]) {
                printf("    first at %u: %08x / %08x (after", i, g_log_orig.w[i], g_log.w[i]);
                for (uint32_t j = i > 6 ? i - 6 : 0; j < i; j++) printf(" %08x", g_log_orig.w[j]);
                printf(")\n");
                break;
            }
            same = false;
        }
        if (!same) {
            differ++; per_bad[kind]++;
            printf("    (pc %s, car %d)\n", g_pc == _PC_24 ? "24" : "53", g_args.car);
            if (differ >= 30) { printf("stopping after 30 differing worlds\n"); break; }
        }
        // the original's writes against the footprint
        if (!fp.replay_only && !fo) {
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
            if (fp.pure && g_log_orig.n) { printf("  FOOTPRINT world %d (%s): pure, but called out\n", it, kind_names[kind]); bad = true; }
            fp_bad += bad;
        }
        // coverage, from the starting state and the original's log
        auto logged = [&](uint32_t tag) { for (uint32_t i = 0; i < g_log_orig.n && i < LOG_MAX; i++) if (g_log_orig.w[i] == tag) return true; return false; };
        auto count = [&](uint32_t tag) { int n = 0; for (uint32_t i = 0; i < g_log_orig.n && i < LOG_MAX; i++) if (g_log_orig.w[i] == tag) n++; return n; };
        // UpdateMessage's footprint claims no texture or skid call when it isn't replay_only
        if ((kind == K_UPDMSG || kind == K_GUPDMSG) && !fp.replay_only && !fo &&
            (logged('BLIT') || logged('ULTX') || logged('RLTX') || logged('SKON') || logged('SKOF') || logged('SKCL'))) {
            printf("  FOOTPRINT world %d (%s): not replay_only, but a texture or skid call\n", it, kind_names[kind]);
            fp_bad++;
        }
        if (logged('CRTF') && c_crt++ < 3) printf("  world %d (%s): the game's CRT fatal-error path (caught)\n", it, kind_names[kind]);
        const CarObject* after = (const CarObject*)(g_arena_after + A_CARS + CAR_STRIDE * g_args.car);
        const CarObject* before = (const CarObject*)(g_arena_snap + A_CARS + CAR_STRIDE * g_args.car);
        if (kind == K_UPDMSG || kind == K_GUPDMSG) {
            if ((uint32_t)after->view_mode < 3) c_mode[after->view_mode]++;
            if (count('BLIT') && logged('RLTX')) {
                // a whole-texture blit is the one with (0, 0, 1, 1); cells otherwise
                for (uint32_t i = 0; i + 6 < g_log_orig.n && i < LOG_MAX - 6; i++)
                    if (g_log_orig.w[i] == 'BLIT') { if (g_log_orig.w[i + 5] == 0x3f800000 && g_log_orig.w[i + 3] == 0 && g_log_orig.w[i + 4] == 0) c_whole++; else c_cells++; break; }
            }
            c_unload += logged('ULTX');
            c_skid_on += logged('SKON'); c_skid_off += logged('SKOF'); c_skid_clear += logged('SKCL');
            c_upd_fp += !fp.replay_only;
        }
        if (kind == K_CTOR || kind == K_GCTOR) {
            c_smoke += logged('SMOK');
            for (uint32_t i = 0; i + 3 < g_log_orig.n && i < LOG_MAX - 3; i++)
                if (g_log_orig.w[i] == 'SKOB') { uint32_t n = g_log_orig.w[i + 2]; (n == 16 || n == 40 ? c_skids1 : c_skids4)++; c_player += n == 40 || n == 160; }
            c_shadow += logged('SHDW'); c_reflect += logged('REFL');
            c_cockpit += after->has_cockpit;
            c_high += after->shadow_model != 0;
        }
        if (kind == K_LOADMODELS || kind == K_CTOR) for (int r = 0; r < 8 && r < after->num_lods; r++) c_lodx += after->lod[r].faces == -1;
        if (kind == K_BEGIN) {
            if (logged('RELT')) { uint8_t f = g_script.format; c_fmt[f == 4 ? 0 : f == 3 ? 1 : 2]++; }
            if (!g_script.grab_ok) c_grabfail++;
            int32_t tod; memcpy(&tod, g_arena_snap + A_OPTS + 4, 4);
            if (tod < 0 || tod > 2) c_tod_panic++; else if (tod == 2) c_night++;
        }
        if (kind == K_END && logged('REPT')) c_fps++;
        if (kind == K_GRAFLOAD || kind == K_FIXGRAF || kind == K_FIXOBJS) {
            for (uint32_t i = 0; i + 1 < g_log_orig.n && i < LOG_MAX - 1; i++)
                if (g_log_orig.w[i] == 'PANC') { uint32_t f = g_log_orig.w[i + 1]; c_badver += f == 0x4f5294; c_dup += f == 0x4f52d4; c_wrongid += f == 0x4f52ec; }
            c_graf_models += count('MCRE');
        }
        if (kind == K_UPDATE) {
            const uintptr_t np = (uintptr_t)after->num_lods_p - (uintptr_t)g_arena;
            const int32_t n = np < ARENA_BYTES - 4 ? *(const int32_t*)(g_arena_after + np) : 0;
            if (!fo) for (int r = 0; r < n && r < 8; r++) if (before->lod[r].dist > after->cam_dist) { c_lod_found++; break; }
            if (((uint32_t*)before)[0] == 0x004dd500) c_ghost_clamp++;
        }
        if (kind == K_ISALPHA) c_alpha[ro & 1]++;
        if (kind == K_FINDSMOKE && (int32_t)ro == -1) c_smoke_none++;
    }
    printf("%d worlds: %d differ, %d faulted, %d changed bytes outside the footprint, %d replay-only (%d set-up constructors faulted)\n",
           iterations, differ, faults, fp_bad, replay_only, g_setup_faults);
    printf("per function (worlds / differing / faulted):\n");
    for (int i = 0; i < N_KINDS; i++) printf("  %-40s %6d %3d %3d\n", kind_names[i], per[i], per_bad[i], per_fault[i]);
    printf("UpdateMessage: view mode 0/1/2 %d/%d/%d, whole blits %d, cell blits %d, skin unloads %d, skid on/off/clear %d/%d/%d, footprint-checked %d\n",
           c_mode[0], c_mode[1], c_mode[2], c_whole, c_cells, c_unload, c_skid_on, c_skid_off, c_skid_clear, c_upd_fp);
    printf("constructor: smoke %d, skids 1/4 %d/%d (player %d), shadow %d, reflection %d, cockpit %d, high-detail shadow model %d; LOD rows with 'x' %d\n",
           c_smoke, c_skids1, c_skids4, c_player, c_shadow, c_reflect, c_cockpit, c_high, c_lodx);
    printf("ViewBegin: sky 565/555/other %d/%d/%d, grab failed %d, night %d, bad time of day %d; ViewEnd fps reports %d\n",
           c_fmt[0], c_fmt[1], c_fmt[2], c_grabfail, c_night, c_tod_panic, c_fps);
    printf("graf: real %d, generated %d; models built %d; panics: bad version %d, duplicate id %d, wrong id %d\n",
           c_real_graf, c_gen_graf, c_graf_models, c_badver, c_dup, c_wrongid);
    printf("Update: a LOD row found ~%d, ghosts %d; IsAlpha 0/1 %d/%d; find_available_smoke none %d\n",
           c_lod_found, c_ghost_clamp, c_alpha[0], c_alpha[1], c_smoke_none);
    return differ || fp_bad ? 1 : 0;
}
