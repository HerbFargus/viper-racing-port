// world_wld_terrain.cpp -- the W1 terrain queries, the BPP / BSP trees and the track loading (hook/wld_terrain.cpp)
// against the originals, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_wld_terrain.cpp
//        /Fo<dir>\ /Fe<dir>\world_wld_terrain.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_wld_terrain.exe [queries per track] [seed] [game dir]
//          (the game dir defaults to ..\game-files\installs\v1.0-RC beside the repo; it is only read)
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. The real tracks: every *.trk in the game dir, and
// Backups\*_original.trk.bak, are opened read-only and their track.bpp / track.bsp payloads read out of the 0TSR
// archive; ResourceGet is a stub that hands a payload to the game's own loader (a fresh copy in a fixed buffer), so
// the trees the queries run on are made by the ORIGINAL load_file / fixup_tree / fixup_nodes. The BSP of every
// stock track is one 200 km triangle, so random BSPs are synthesised too (random triangles with their edge normals,
// random trees, degenerate triangles), and loaded the same way. LogReport / LogPanic, the profiler timer, the sky,
// the graphics loader and ResourceForget are logging stubs.
//
// Each check runs the original, restores the state, runs the rewrite, and compares the return value, the arguments'
// memory, the world statics (bpp.obj's header and call counter, bsp.obj's head, counters, hit list and sphere
// contact, track.obj's graphics handle, terrain.obj's timer), the BSP's own bytes (the visited marks), the stubs'
// logs and any fault. The FPU runs as the physics thread runs it (single precision), a quarter of the checks at
// double precision (the main thread's queries), and a quarter with zero-divide and overflow unmasked (the physics
// thread's exceptions). The game's CRT never ran its start-up here, so its fatal-message routines (__amsg_exit,
// __NMSG_WRITE, __FF_MSGBANNER, __crtMessageBoxA) are patched to log and exit: no dialog can appear, and the game's
// sprintf is never reached (LogReport / LogPanic are stubs). "isolated": the rewrite alone, its callees the originals; "chain": every original entry
// patched to jump to its rewrite, so the whole rewritten call tree runs (recursion included).
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#include "../hook/port.h"
#undef PORT_FN_BUILDS
struct Reg { uint32_t v10; void* fn; const char* name; uint8_t saved[5]; };
static Reg g_regs[96];
static int g_nregs;
struct RegAdd {
    RegAdd(uint32_t v, void* f, const char* n) { g_regs[g_nregs].v10 = v; g_regs[g_nregs].fn = f; g_regs[g_nregs].name = n; g_nregs++; }
};
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN) static RegAdd VP_CAT(reg_, NEW)(V10, (void*)&NEW, NAME);

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

#include "../hook/wld_terrain.cpp"

// ---- random values -------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float rf(float lo, float hi) { return lo + (hi - lo) * uni(); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t ubits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float special() {
    switch (rnd() % 9) {
    case 0: return fbits(0x7fc00000u | (rnd() & 0x3fffff));        // quiet NaN
    case 1: return fbits(0xffc00000u);
    case 2: return fbits(0x7f800000u);                              // +inf
    case 3: return fbits(0xff800000u);
    case 4: return fbits(0x80000000u);                              // -0
    case 5: return fbits(rnd() & 0x807fffff);                       // denormal
    case 6: return chance(50) ? 3e38f : -3e38f;                     // huge
    case 7: return fbits(0x7f800001u | (rnd() & 0x3fffff));         // signalling NaN
    default: return fbits(rnd());                                   // any bits
    }
}

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
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);   // (inherited by the child)
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
static bool g_chain_on;
static void set_chain(bool on) {
    if (on == g_chain_on) return;
    for (int i = 0; i < g_nregs; i++) {
        if (on) patch_jmp(g_regs[i].v10, g_regs[i].fn);
        else memcpy((void*)(uintptr_t)g_regs[i].v10, g_regs[i].saved, 5);
    }
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
    g_chain_on = on;
}

// ---- the stubs' log --------------------------------------------------------------------------------------------------
enum { LOG_MAX = 4096 };
static uint32_t g_log[2][LOG_MAX];
static int g_nlog[2], g_pass;
static void log_put(uint32_t v) { if (g_nlog[g_pass] < LOG_MAX) g_log[g_pass][g_nlog[g_pass]] = v; g_nlog[g_pass]++; }
static void log_str(const char* s) {                             // a string argument: its address and its text
    log_put((uint32_t)(uintptr_t)s);
    if (!s) return;
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    log_put(h);
}
static void __cdecl stub_log_report(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_put(0x10600001);
    log_put((uint32_t)(uintptr_t)fmt);
    if (fmt == (const char*)0x004f5128 || fmt == (const char*)0x004f51ec)
        for (int i = 0; i < 4; i++) log_put(va_arg(ap, uint32_t));
    va_end(ap);
}
static void __cdecl stub_log_panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_put(0x10600002);
    log_put((uint32_t)(uintptr_t)fmt);
    if (fmt == (const char*)0x004f50e0 || fmt == (const char*)0x004f53e4) log_str(va_arg(ap, const char*));
    va_end(ap);
}
static int32_t g_multi_id, g_graf_handle;
static int __cdecl stub_multi_begin(const char* name) { log_put(0x10600003); log_str(name); return g_multi_id; }
static void __cdecl stub_multi_end(int id, const char* a, int b) { log_put(0x10600004); log_put(id); log_put((uint32_t)(uintptr_t)a); log_put(b); }
static void __cdecl stub_view_set_sky(uint32_t a, uint32_t b) { log_put(0x10600005); log_put(a); log_put(b); }
static void __cdecl stub_graf_min_lod(uint32_t a) { log_put(0x10600006); log_put(a); }
static int __cdecl stub_graf_load(const char* name) { log_put(0x10600007); log_str(name); return g_graf_handle; }
static void __cdecl stub_graf_unload(int h) { log_put(0x10600008); log_put(h); }
// the game's CRT must never put up a message box (its start-up never ran here): its fatal-message routines log and exit
static void __cdecl stub_crt_fatal(int code) {
    printf("the game's CRT raised a fatal message (%d): stopping\n", code);
    ExitProcess(3);
}
static uint8_t __cdecl stub_resource_forget(void* p) { log_put(0x10600009); log_put((uint32_t)(uintptr_t)p); return 1; }

// ResourceGet: a payload for 'BPPT' or 'BSPT', copied into its fixed buffer when fresh
struct Res {
    const uint8_t* payload;
    uint32_t size;               // the payload's bytes
    int32_t claimed;             // the size ResourceGet reports
    uint32_t version;
    uint8_t fresh, fail;
    uint8_t* buf;                // where the game gets it
    uint32_t cap;
};
static Res g_res[2];                                             // 0 bpp, 1 bsp
static void* __cdecl stub_resource_get(const char* name, uint32_t type, uint32_t* ver, int32_t* size, uint8_t* x, uint8_t* fresh) {
    log_put(0x1060000a);
    log_str(name);
    log_put(type);
    log_put((uint32_t)(uintptr_t)x);
    Res* r = type == 0x42505054u ? &g_res[0] : type == 0x42535054u ? &g_res[1] : 0;
    if (!r || r->fail || !r->buf) return 0;
    if (r->fresh) memcpy(r->buf, r->payload, r->size);
    *ver = r->version;
    *size = r->claimed;
    *fresh = r->fresh;
    return r->buf;
}

// ---- the state a check compares ------------------------------------------------------------------------------------
struct Arena {
    P3 p[8];
    int32_t i[4];
    BPPFinder finder;
    float f[4];
    char name[16];
    uint32_t ret;                                                // the return value
};
static Arena g_arena;
struct Region { uint8_t* p; uint32_t n; const char* what; };
static Region g_regions[16];
static int g_nregions;
static void regions_reset() {
    g_nregions = 0;
    g_regions[g_nregions++] = {(uint8_t*)&g_arena, sizeof g_arena, "arena"};
    g_regions[g_nregions++] = {(uint8_t*)0x004f4c78, 4, "terrain timer"};
    g_regions[g_nregions++] = {(uint8_t*)0x004f50c0, 0xc, "bpp header / counter"};
    g_regions[g_nregions++] = {(uint8_t*)0x004f5348, 0x44, "bsp head / counters"};
    g_regions[g_nregions++] = {(uint8_t*)0x00557fc8, 4, "track graf"};
    g_regions[g_nregions++] = {(uint8_t*)0x00559dc8, 0x164, "bsp statics"};
}
static void regions_add(void* p, uint32_t n, const char* what) { g_regions[g_nregions++] = {(uint8_t*)p, n, what}; }
static std::vector<uint8_t> g_start, g_after_orig;
static void snap_save(std::vector<uint8_t>& v) {
    size_t t = 0;
    for (int i = 0; i < g_nregions; i++) t += g_regions[i].n;
    v.resize(t);
    t = 0;
    for (int i = 0; i < g_nregions; i++) { memcpy(&v[t], g_regions[i].p, g_regions[i].n); t += g_regions[i].n; }
}
static void snap_load(const std::vector<uint8_t>& v) {
    size_t t = 0;
    for (int i = 0; i < g_nregions; i++) { memcpy(g_regions[i].p, &v[t], g_regions[i].n); t += g_regions[i].n; }
}
static bool snap_same(const std::vector<uint8_t>& v, int* region, uint32_t* off) {
    size_t t = 0;
    for (int i = 0; i < g_nregions; i++) {
        if (memcmp(g_regions[i].p, &v[t], g_regions[i].n)) {
            for (uint32_t k = 0; k < g_regions[i].n; k++)
                if (g_regions[i].p[k] != v[t + k]) { *region = i; *off = k; break; }
            return false;
        }
        t += g_regions[i].n;
    }
    return true;
}

// ---- running a check -------------------------------------------------------------------------------------------------
// A thunk makes one call from the arena and stores its return value in g_arena.ret; orig: the original's address
// (with the chain on, that is the whole rewritten tree), otherwise the rewrite itself.
typedef void (*Thunk)(bool orig);
enum FpMode { FP_PHYS, FP_MAIN, FP_PHYS_EXC };
static unsigned g_code;
static int filt(EXCEPTION_POINTERS* e) { g_code = e->ExceptionRecord->ExceptionCode; return EXCEPTION_EXECUTE_HANDLER; }
static int run_pass(Thunk th, bool orig, FpMode fm, unsigned* code) {
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, fm == FP_MAIN ? _PC_53 : _PC_24, _MCW_PC);
    if (fm == FP_PHYS_EXC) _controlfp_s(&cw, _EM_INVALID | _EM_UNDERFLOW | _EM_INEXACT | _EM_DENORMAL, _MCW_EM);
    int fault = 0;
    g_code = 0;
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] {
            th(orig);
            volatile float z = 1.0f;
            z = z + 0.0f;                                        // a pending x87 exception is raised here
        }, filt)) { fault = 1; _fpreset(); }
#else
    __try {
        th(orig);
        volatile float z = 1.0f;
        z = z + 0.0f;                                            // a pending x87 exception is raised here
    } __except (filt(GetExceptionInformation())) { fault = 1; _fpreset(); }
#endif
    _controlfp_s(&cw, _CW_DEFAULT, 0xffffffff);
    _clearfp();
    *code = fault ? g_code : 0;
    return fault;
}

struct Tally { const char* name; long n, bad, faults, hits; };
static Tally g_tally[128];
static int g_ntally;
static Tally* tally(const char* name) {
    for (int i = 0; i < g_ntally; i++) if (!strcmp(g_tally[i].name, name)) return &g_tally[i];
    g_tally[g_ntally] = {name, 0, 0, 0, 0};
    return &g_tally[g_ntally++];
}
static bool g_chain;                                             // this round: chain mode
static FpMode pick_fp() { uint32_t k = rnd() % 4; return k == 0 ? FP_MAIN : k == 1 ? FP_PHYS_EXC : FP_PHYS; }

// one check: 1 differs
static int check(const char* name, Thunk th, FpMode fm) {
    Tally* t = tally(name);
    t->n++;
    g_arena.ret = 0;
    snap_save(g_start);
    g_pass = 0; g_nlog[0] = 0;
    unsigned co, cn;
    int fo = run_pass(th, true, fm, &co);
    snap_save(g_after_orig);
    snap_load(g_start);
    g_pass = 1; g_nlog[1] = 0;
    if (g_chain) set_chain(true);
    int fnw = run_pass(th, g_chain, fm, &cn);
    if (g_chain) set_chain(false);
    t->faults += fo;
    if (!fo && g_arena.ret) t->hits++;
    int region = -1;
    uint32_t off = 0;
    bool same = fo == fnw && co == cn;
    if (same && !fo) {
        int nl = g_nlog[0] < LOG_MAX ? g_nlog[0] : LOG_MAX;
        same = snap_same(g_after_orig, &region, &off) && g_nlog[0] == g_nlog[1] && !memcmp(g_log[0], g_log[1], 4 * nl);
    }
    if (!same && t->bad++ < 4) {
        printf("  MISMATCH %s%s (check %ld, fp mode %d)\n", name, g_chain ? " [chain]" : "", t->n, (int)fm);
        if (fo != fnw || co != cn) printf("    fault: original %d (%08x), rewrite %d (%08x)\n", fo, co, fnw, cn);
        if (region >= 0) {
            size_t base = 0;
            for (int i = 0; i < region; i++) base += g_regions[i].n;
            uint32_t k0 = off & ~3u;
            for (uint32_t k = k0; k < k0 + 16 && k + 4 <= g_regions[region].n; k += 4) {
                uint32_t a, b;
                memcpy(&a, &g_after_orig[base + k], 4);
                memcpy(&b, g_regions[region].p + k, 4);
                printf("    %s+0x%x original %08x (%.9g), rewrite %08x (%.9g)%s\n", g_regions[region].what, k, a, fbits(a), b,
                       fbits(b), a != b ? "  <--" : "");
            }
        }
        if (g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], 4 * (g_nlog[0] < LOG_MAX ? g_nlog[0] : LOG_MAX)))
            printf("    stub logs: %d / %d entries\n", g_nlog[0], g_nlog[1]);
    }
    snap_load(g_after_orig);                                     // carry on from the original's result
    return same ? 0 : 1;
}

// ---- the tracks ------------------------------------------------------------------------------------------------------
struct Track { std::string name; std::vector<uint8_t> bpp, bsp; uint32_t bpp_ver, bsp_ver; };
static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");                         // read-only
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    size_t got = fread(out.data(), 1, n, f);
    fclose(f);
    return got == (size_t)n;
}
// the 0TSR archive: a directory of 36-byte entries (name[16], tag, version, size, 8 reserved), then each payload
// behind 8 bytes (reserved, "!IGM"), in directory order
static bool read_track(const std::string& path, Track& t) {
    std::vector<uint8_t> d;
    if (!read_file(path, d) || d.size() < 16 || memcmp(d.data(), "0TSR", 4)) return false;
    int32_t n;
    memcpy(&n, &d[4], 4);
    size_t cur = 16 + 36 * (size_t)n;
    bool got_bpp = false, got_bsp = false;
    for (int i = 0; i < n && cur + 8 <= d.size(); i++) {
        const uint8_t* e = &d[16 + 36 * i];
        char nm[17] = {0};
        memcpy(nm, e, 16);
        int32_t ver, size;
        memcpy(&ver, e + 20, 4);
        memcpy(&size, e + 24, 4);
        if (memcmp(&d[cur + 4], "!IGM", 4)) break;
        if (cur + 8 + size > d.size()) break;
        if (!_stricmp(nm, "track.bpp")) { t.bpp.assign(d.begin() + cur + 8, d.begin() + cur + 8 + size); t.bpp_ver = ver; got_bpp = true; }
        if (!_stricmp(nm, "track.bsp")) { t.bsp.assign(d.begin() + cur + 8, d.begin() + cur + 8 + size); t.bsp_ver = ver; got_bsp = true; }
        cur += 8 + size;
    }
    return got_bpp && got_bsp;
}

// ---- the live BPP (made by the original loader) ------------------------------------------------------------------
static uint8_t* g_bppbuf;                                        // the live tree
static uint8_t* g_ldbuf;                                         // loader tests
static uint32_t g_bufcap = 8u << 20;
static bpp_header* g_hdr;
static std::vector<float> g_box;                                 // min x, max x, min z, max z, min y, max y

static const bpp_tri* rtri() { return g_hdr->tris + rnd() % (uint32_t)g_hdr->n_tris; }
static P3 on_tri3(const P3* v, int mode) {
    double w[3];
    switch (mode) {
    case 0: { double a = uni(), b = uni(); if (a + b > 1) { a = 1 - a; b = 1 - b; } w[0] = a; w[1] = b; w[2] = 1 - a - b; break; }
    case 1: { int k = rnd() % 3; double a = uni(); w[k] = 0; w[(k + 1) % 3] = a; w[(k + 2) % 3] = 1 - a; break; }
    case 2: { int k = rnd() % 3; w[0] = w[1] = w[2] = 0; w[k] = 1; break; }
    case 3: { int k = rnd() % 3; double e = rf(1e-7f, 1e-3f); double a = uni(); w[k] = -e; w[(k + 1) % 3] = a + e / 2; w[(k + 2) % 3] = 1 - a + e / 2; break; }
    default: w[0] = w[1] = w[2] = 1.0 / 3; break;
    }
    P3 p;
    p.x = (float)(w[0] * v[0].x + w[1] * v[1].x + w[2] * v[2].x);
    p.y = (float)(w[0] * v[0].y + w[1] * v[1].y + w[2] * v[2].y);
    p.z = (float)(w[0] * v[0].z + w[1] * v[1].z + w[2] * v[2].z);
    return p;
}
static float plane_y(const bpp_tri* t, float x, float z) {
    if (fabs(t->n.y) < 1e-6) return t->v[0].y;
    return (float)(-(t->n.x * (double)x + t->n.z * (double)z + t->d) / t->n.y);
}
static P3 ground_point(const bpp_tri** tt = 0) {
    const bpp_tri* t = rtri();
    if (tt) *tt = t;
    P3 p = on_tri3(t->v, rnd() % 5);
    if (chance(50)) p.y = plane_y(t, p.x, p.z);
    return p;
}
static const float k_offsets[] = {0, 1e-4f, -1e-4f, 0.001f, -0.001f, 0.0005f, -0.0005f, 0.3f, -0.3f, 2, -2, 50, -50, 1e-6f};
static P3 rpoint() {
    uint32_t k = rnd() % 100;
    P3 p;
    if (k < 60) {
        p = ground_point();
        p.y += k_offsets[rnd() % (sizeof k_offsets / sizeof *k_offsets)];
    } else if (k < 80) {
        p = {rf(g_box[0], g_box[1]), rf(g_box[4] - 5, g_box[5] + 5), rf(g_box[2], g_box[3])};
    } else if (k < 85) {
        p = {rf(-1e5f, 1e5f), rf(-1e3f, 1e3f), rf(-1e5f, 1e5f)};
    } else if (k < 92) {                                         // on a millimetre rounding boundary
        p = ground_point();
        p.x = (float)((floor(p.x / 0.001) + 0.5) * 0.001);
        if (chance(50)) p.z = (float)((floor(p.z / 0.001) + 0.5) * 0.001);
        if (chance(30)) p.x = fbits(ubits(p.x) + (rnd() % 3) - 1);
    } else if (k < 96) {
        p = ground_point();
        float* c = &p.x + rnd() % 3;
        *c = special();
    } else {                                                     // huge but finite: sums past FLT_MAX (an overflow
        p = ground_point();                                      // when stored, a fault on the physics thread)
        float* c = &p.x + rnd() % 3;
        *c = (chance(50) ? 1 : -1) * rf(1e36f, 3.4e38f);
    }
    return p;
}
static void rsegment(P3* a, P3* b) {
    uint32_t k = rnd() % 100;
    if (k < 55) {                                                // a wheel / corner ray: above to below
        P3 g = ground_point();
        *a = {g.x + rf(-0.05f, 0.05f), g.y + rf(0.01f, 3), g.z + rf(-0.05f, 0.05f)};
        *b = {g.x + rf(-0.3f, 0.3f), g.y - rf(0.01f, 3), g.z + rf(-0.3f, 0.3f)};
        if (chance(30)) std::swap(*a, *b);
    } else if (k < 68) {
        *a = rpoint();
        *b = {a->x + rf(-40, 40), a->y + rf(-10, 10), a->z + rf(-40, 40)};
    } else if (k < 76) {
        *a = rpoint();
        *b = *a;
        b->y -= rf(-5, 5);                                       // vertical
    } else if (k < 80) {
        *a = rpoint();
        *b = *a;                                                 // a point
    } else if (k < 88) {                                         // an end on the plane
        const bpp_tri* t;
        P3 g = ground_point(&t);
        g.y = plane_y(t, g.x, g.z);
        *a = g;
        *b = {g.x + rf(-0.5f, 0.5f), g.y + rf(-3, 3), g.z + rf(-0.5f, 0.5f)};
        if (chance(50)) std::swap(*a, *b);
    } else if (k < 94) {
        *a = {rf(g_box[0], g_box[1]), rf(g_box[4] - 5, g_box[5] + 5), rf(g_box[2], g_box[3])};
        *b = {rf(g_box[0], g_box[1]), rf(g_box[4] - 5, g_box[5] + 5), rf(g_box[2], g_box[3])};
    } else {
        rsegment(a, b);
        float* c = (chance(50) ? &a->x : &b->x) + rnd() % 3;
        *c = special();
    }
}
static void garbage(void* p, size_t n) {
    uint8_t* b = (uint8_t*)p;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd();
}

// ---- the thunks: the BPP queries ----------------------------------------------------------------------------------------
#define ORIG(T, a) ((T)(uintptr_t)(a))
static Arena& Ar = g_arena;
static void th_height_xz(bool o) {
    float r = o ? ORIG(float(__cdecl*)(float, float), 0x465ae0)(Ar.p[0].x, Ar.p[0].z) : TerrainGetHeightXZ(Ar.p[0].x, Ar.p[0].z);
    Ar.ret = ubits(r);
}
static void th_height_p(bool o) { Ar.ret = o ? ORIG(uint8_t(__cdecl*)(P3*), 0x465b30)(&Ar.p[0]) : TerrainGetHeightP(&Ar.p[0]); }
static void th_height_ps(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__cdecl*)(P3*, int32_t*), 0x465b50)(&Ar.p[0], &Ar.i[0]) : TerrainGetHeightPSurface(&Ar.p[0], &Ar.i[0]);
}
static void th_intersection4(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__cdecl*)(P3*, P3*, P3*, P3*), 0x465b90)(&Ar.p[0], &Ar.p[1], &Ar.p[2], &Ar.p[3])
               : TerrainGetIntersection4(&Ar.p[0], &Ar.p[1], &Ar.p[2], &Ar.p[3]);
}
static int32_t* g_surf_arg;
static void th_intersection5(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__cdecl*)(P3*, P3*, P3*, P3*, int32_t*), 0x465be0)(&Ar.p[0], &Ar.p[1], &Ar.p[2], &Ar.p[3], g_surf_arg)
               : TerrainGetIntersection5(&Ar.p[0], &Ar.p[1], &Ar.p[2], &Ar.p[3], g_surf_arg);
}
static void th_height5(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__cdecl*)(float, float, P3*, P3*, int32_t*), 0x465c40)(Ar.p[0].x, Ar.p[0].z, &Ar.p[2], &Ar.p[3], &Ar.i[0])
               : TerrainGetHeightXZPNS(Ar.p[0].x, Ar.p[0].z, &Ar.p[2], &Ar.p[3], &Ar.i[0]);
}
static void th_sphere(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__cdecl*)(P3*, float, P3*, P3*, int32_t*), 0x465ca0)(&Ar.p[0], Ar.f[0], &Ar.p[2], &Ar.p[3], &Ar.i[0])
               : TerrainGetSphereIntersection(&Ar.p[0], Ar.f[0], &Ar.p[2], &Ar.p[3], &Ar.i[0]);
}
static P3 *g_a0, *g_a1, *g_a2;                                   // argument pointers (they may alias)
static void th_hit_xz(bool o) {
    Ar.ret = (uint32_t)(uintptr_t)(o ? ORIG(bpp_tri*(__cdecl*)(P3*, P3*), 0x46d790)(g_a0, g_a1) : BPPHitXZ(g_a0, g_a1));
}
static void th_find_point(bool o) { Ar.ret = o ? ORIG(int32_t(__cdecl*)(P3*), 0x46d740)(g_a0) : bpp_find_point(g_a0); }
static void th_bpp_hit(bool o) {
    Ar.ret = (uint32_t)(uintptr_t)(o ? ORIG(bpp_tri*(__cdecl*)(P3*, P3*, P3*), 0x46d0c0)(g_a0, g_a1, g_a2) : BPPHit(g_a0, g_a1, g_a2));
}
static void th_finder_find(bool o) {
    Ar.ret = (uint32_t)(uintptr_t)(o ? ORIG(bpp_tri*(__fastcall*)(BPPFinder*, int), 0x46d0a0)(&Ar.finder, 0) : BPPFinder_Find(&Ar.finder, 0));
}
static bpp_node* g_node_arg;
static int32_t g_int_arg;
static void th_finder_bpp_find(bool o) {
    Ar.ret = o ? ORIG(int32_t(__fastcall*)(BPPFinder*, int, bpp_node*), 0x46d390)(&Ar.finder, 0, g_node_arg)
               : BPPFinder_bpp_find(&Ar.finder, 0, g_node_arg);
}
static void th_finder_test_poly(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__fastcall*)(BPPFinder*, int, int32_t), 0x46d360)(&Ar.finder, 0, g_int_arg)
               : BPPFinder_test_poly(&Ar.finder, 0, g_int_arg);
}
static void th_finder_intersect_plane(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__fastcall*)(BPPFinder*, int), 0x46d0f0)(&Ar.finder, 0) : BPPFinder_intersect_plane(&Ar.finder, 0);
}
static void th_finder_point_in_poly(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__fastcall*)(BPPFinder*, int), 0x46d290)(&Ar.finder, 0) : BPPFinder_point_in_poly(&Ar.finder, 0);
}

static void pick_args3() {                                       // three P3 arguments from the arena, sometimes aliased
    g_a0 = &Ar.p[0]; g_a1 = &Ar.p[1]; g_a2 = &Ar.p[2];
    if (chance(8)) g_a2 = chance(50) ? g_a0 : g_a1;
}
static void setup_finder() {
    garbage(&Ar.finder, sizeof Ar.finder);
    Ar.finder.p0 = &Ar.p[0];
    Ar.finder.p1 = &Ar.p[1];
    Ar.finder.out = chance(8) ? &Ar.p[0] : &Ar.p[2];
}

// a triangle standing on edge (or with a NaN normal) under the query point: BPPHitXZ's rejection
static bpp_tri* g_steep;
static float g_steep_ny;
static void steep_begin(P3* p) {
    const bpp_tri* t;
    *p = ground_point(&t);
    g_steep = (bpp_tri*)t;
    g_steep_ny = t->n.y;
    static const uint32_t ny[] = {0, 0x80000000u, 0x34000000u, 0xb4000000u, 0x34000001u, 0x33ffffffu, 0x7fc00000u, 0x2f000000u};
    g_steep->n.y = fbits(ny[rnd() % 8]);
}
static void steep_end() { if (g_steep) g_steep->n.y = g_steep_ny; g_steep = 0; }

static void bpp_queries(int nq) {
    const int ntri = g_hdr->n_tris, nnode = g_hdr->n_nodes;
    for (int q = 0; q < nq; q++) {
        garbage(&Ar, sizeof Ar);
        memcpy(Ar.name, "track", 6);
        FpMode fm = pick_fp();
        P3 a, b;
        bool steep = (q % 16 == 0 || q % 16 == 5 || q % 16 == 7) && chance(10);
        switch (q % 16) {
        case 0: a = rpoint(); if (steep) steep_begin(&a); Ar.p[0] = a; check("TerrainGetHeight(x,z)", th_height_xz, fm); break;
        case 1: Ar.p[0] = rpoint(); check("TerrainGetHeight(point)", th_height_p, fm); break;
        case 2: Ar.p[0] = rpoint(); check("TerrainGetHeight(point,surface)", th_height_ps, fm); break;
        case 3: rsegment(&a, &b); Ar.p[0] = a; Ar.p[1] = b; check("TerrainGetIntersection(4)", th_intersection4, fm); break;
        case 4:
            rsegment(&a, &b); Ar.p[0] = a; Ar.p[1] = b;
            g_surf_arg = chance(20) ? 0 : &Ar.i[0];
            check("TerrainGetIntersection(5)", th_intersection5, fm);
            break;
        case 5: Ar.p[0] = rpoint(); if (steep) steep_begin(&Ar.p[0]); check("TerrainGetHeight(x,z,point,normal,surface)", th_height5, fm); break;
        case 6: {
            const bpp_tri* t;
            P3 g = ground_point(&t);
            g.y = plane_y(t, g.x, g.z);
            float r = chance(10) ? special() : rf(0.05f, 2.5f);
            double s = chance(15) ? (double)r * (1.0 - (double)(rnd() % 8) * 6e-8) : (double)r * rf(-1.4f, 1.4f);
            if (chance(10)) s = rf(-100, 100);
            Ar.p[0] = {(float)(g.x + t->n.x * s), (float)(g.y + t->n.y * s), (float)(g.z + t->n.z * s)};
            if (chance(5)) Ar.p[0] = rpoint();
            Ar.f[0] = r;
            check("TerrainGetSphereIntersection", th_sphere, fm);
            break;
        }
        case 7:
            pick_args3(); Ar.p[0] = rpoint(); if (chance(10)) g_a1 = g_a0;
            if (steep) steep_begin(&Ar.p[0]);
            check("BPPHitXZ", th_hit_xz, fm);
            break;
        case 8: pick_args3(); Ar.p[0] = rpoint(); check("bpp_find_point", th_find_point, fm); break;
        case 9: pick_args3(); rsegment(&Ar.p[0], &Ar.p[1]); check("BPPHit", th_bpp_hit, fm); break;
        case 10: setup_finder(); rsegment(&Ar.p[0], &Ar.p[1]); check("BPPFinder::Find", th_finder_find, fm); break;
        case 11:
            setup_finder(); rsegment(&Ar.p[0], &Ar.p[1]);
            g_node_arg = g_hdr->nodes + rnd() % (uint32_t)nnode;
            check("BPPFinder::bpp_find", th_finder_bpp_find, fm);
            break;
        case 12: case 13: {
            setup_finder();
            const bpp_tri* t = rtri();
            int32_t idx = (int32_t)(t - g_hdr->tris);
            P3 g = on_tri3(t->v, rnd() % 5);
            g.y = plane_y(t, g.x, g.z);
            Ar.p[0] = {g.x + rf(-0.2f, 0.2f), g.y + rf(-2, 2), g.z + rf(-0.2f, 0.2f)};
            Ar.p[1] = {g.x + rf(-0.2f, 0.2f), g.y + rf(-2, 2), g.z + rf(-0.2f, 0.2f)};
            if (chance(15)) Ar.p[0] = g;
            if (chance(15)) Ar.p[1] = g;
            if (chance(5)) rsegment(&Ar.p[0], &Ar.p[1]);
            if (q % 16 == 12) {
                g_int_arg = chance(10) ? -1 : idx;
                check("BPPFinder::test_poly", th_finder_test_poly, fm);
            } else {
                Ar.finder.tri = idx;
                check("BPPFinder::intersect_plane", th_finder_intersect_plane, fm);
            }
            break;
        }
        case 14: {
            setup_finder();
            const bpp_tri* t = rtri();
            Ar.finder.tri = (int32_t)(t - g_hdr->tris);
            P3 g = on_tri3(t->v, rnd() % 5);
            if (chance(10)) g = rpoint();
            *Ar.finder.out = g;
            check("BPPFinder::point_in_poly", th_finder_point_in_poly, fm);
            break;
        }
        default: {                                               // any triangle index for the finder's low-level tests
            setup_finder();
            rsegment(&Ar.p[0], &Ar.p[1]);
            Ar.finder.tri = rnd() % (uint32_t)ntri;
            *Ar.finder.out = rpoint();
            if (chance(50)) check("BPPFinder::intersect_plane", th_finder_intersect_plane, fm);
            else check("BPPFinder::point_in_poly", th_finder_point_in_poly, fm);
            break;
        }
        }
        steep_end();
    }
}

// ---- loaders -----------------------------------------------------------------------------------------------------------
static void th_bpp_load(bool o) { if (o) ORIG(void(__cdecl*)(const char*), 0x46d070)(Ar.name); else BPPLoadRw(Ar.name); }
static void th_bpp_load_file(bool o) {
    Ar.ret = (uint32_t)(uintptr_t)(o ? ORIG(bpp_header*(__cdecl*)(const char*), 0x46d540)(Ar.name) : bpp_load_file(Ar.name));
}
static void th_bpp_unload(bool o) { if (o) ORIG(void(__cdecl*)(), 0x46d090)(); else BPPUnloadRw(); }
static void th_bpp_unload_file(bool o) { if (o) ORIG(void(__cdecl*)(), 0x46d600)(); else bpp_unload_file(); }
static bpp_node *g_ft_root, *g_ft_base;
static int32_t g_ft_limit;
static void th_fixup_tree(bool o) {
    if (o) ORIG(void(__cdecl*)(bpp_node*, bpp_node*, int32_t), 0x46d610)(g_ft_root, g_ft_base, g_ft_limit);
    else fixup_tree(g_ft_root, g_ft_base, g_ft_limit);
}
static void th_bsp_load(bool o) { if (o) ORIG(void(__cdecl*)(const char*), 0x46fb50)(Ar.name); else BSPLoadRw(Ar.name); }
static void th_bsp_load_file(bool o) {
    Ar.ret = (uint32_t)(uintptr_t)(o ? ORIG(bsp_node*(__cdecl*)(const char*), 0x470be0)(Ar.name) : bsp_load_file(Ar.name));
}
static void th_bsp_unload(bool o) { if (o) ORIG(void(__cdecl*)(), 0x46fbc0)(); else BSPUnloadRw(); }
static void th_bsp_unload_file(bool o) { if (o) ORIG(void(__cdecl*)(), 0x470c60)(); else bsp_unload_file(); }
static bsp_node* g_fn_node;
static int32_t g_fn_base;
static void th_fixup_nodes(bool o) {
    if (o) ORIG(void(__cdecl*)(bsp_node*, int32_t), 0x46fb70)(g_fn_node, g_fn_base);
    else fixup_nodes(g_fn_node, g_fn_base);
}
static void th_track_load(bool o) { Ar.ret = o ? ORIG(uint8_t(__cdecl*)(const char*), 0x469500)(Ar.name) : TrackLoad(Ar.name); }
static void th_track_unload(bool o) { if (o) ORIG(void(__cdecl*)(), 0x469580)(); else TrackUnload(); }
static void th_track_begin(bool o) { if (o) ORIG(void(__cdecl*)(), 0x469590)(); else track_begin(); }
static void th_track_end(bool o) { if (o) ORIG(void(__cdecl*)(), 0x4695d0)(); else track_end(); }
static void th_terrain_begin(bool o) { if (o) ORIG(void(__cdecl*)(), 0x465aa0)(); else TerrainBegin(); }
static void th_terrain_end(bool o) { if (o) ORIG(void(__cdecl*)(), 0x465ac0)(); else TerrainEnd(); }

static void loader_tests(const Track& t, std::vector<uint8_t>& bsp_payload, uint32_t bsp_ver) {
    Res& rb = g_res[0];
    Res& rs = g_res[1];
    rb = {t.bpp.data(), (uint32_t)t.bpp.size(), (int32_t)t.bpp.size(), t.bpp_ver, 1, 0, g_ldbuf, g_bufcap};
    rs = {bsp_payload.data(), (uint32_t)bsp_payload.size(), (int32_t)bsp_payload.size(), bsp_ver, 1, 0, g_ldbuf + g_bufcap / 2, 0};
    regions_reset();
    regions_add(g_ldbuf, (uint32_t)t.bpp.size(), "bpp buffer");
    regions_add(g_ldbuf + g_bufcap / 2, (uint32_t)bsp_payload.size(), "bsp buffer");
    for (int v = 0; v < 9; v++) {
        garbage(&Ar, sizeof Ar);
        const char* names[] = {"track.bpp", "track.bsp", "nfield", "NFIELD", "heaven", "bemidji", "", "Heaven", "kenyon"};
        strcpy(Ar.name, names[v % 9]);
        rb.fresh = rs.fresh = 1; rb.fail = rs.fail = 0; rb.version = t.bpp_ver; rs.version = bsp_ver;
        rb.claimed = (int32_t)t.bpp.size();
        if (v == 3) rb.fresh = rs.fresh = 0;                         // already loaded: no fixups
        if (v == 4) { rb.version = 7; rs.version = 3; }              // a bad version: LogPanic, then fixed up anyway
        if (v == 5) rb.fail = rs.fail = 1;                           // not found
        if (v >= 6) {                                                // the node section claimed tiny: fixup_tree reports
            int32_t ntri;                                            // (and fixes up all the same)
            memcpy(&ntri, t.bpp.data(), 4);
            rb.claimed = 0x14 + ntri * 56 + (int32_t)(rnd() % 64);
        }
        if (v == 3) {                                                // pre-load, fixed up, for the not-fresh path
            memcpy(g_ldbuf, t.bpp.data(), t.bpp.size());
            memcpy(g_ldbuf + g_bufcap / 2, bsp_payload.data(), bsp_payload.size());
        }
        FpMode fm = FP_PHYS;
        check("BPPLoad", th_bpp_load, fm);
        check("load_file(bpp.obj)", th_bpp_load_file, fm);
        check("BSPLoad", th_bsp_load, fm);
        check("load_file(bsp.obj)", th_bsp_load_file, fm);
        g_multi_id = (int32_t)rnd();
        g_graf_handle = (int32_t)rnd();
        check("TrackLoad", th_track_load, fm);
        check("track_begin", th_track_begin, fm);
        check("TerrainBegin", th_terrain_begin, fm);
        check("TerrainEnd", th_terrain_end, fm);
        check("track_end", th_track_end, fm);
        check("TrackUnload", th_track_unload, fm);
        check("BPPUnload", th_bpp_unload, fm);
        check("unload_file(bpp.obj)", th_bpp_unload_file, fm);
        check("BSPUnload", th_bsp_unload, fm);
        check("unload_file(bsp.obj)", th_bsp_unload_file, fm);
    }
    // fixup_tree and fixup_nodes on their own, from a raw payload with the header's pointers made by hand
    for (int v = 0; v < 3; v++) {
        memcpy(g_ldbuf, t.bpp.data(), t.bpp.size());
        bpp_header* h = (bpp_header*)g_ldbuf;
        h->tris = (bpp_tri*)(g_ldbuf + 0x14);
        h->nodes = (bpp_node*)(g_ldbuf + 0x14 + h->n_tris * 56);
        h->root = h->nodes + (uint32_t)(uintptr_t)h->root;
        g_ft_root = h->root;
        g_ft_base = h->nodes;
        g_ft_limit = v == 0 ? (int32_t)(t.bpp.size() - 0x14 - h->n_tris * 56) : v == 1 ? (int32_t)(rnd() % 200) : -1;
        check("fixup_tree", th_fixup_tree, FP_PHYS);
        memcpy(g_ldbuf + g_bufcap / 2, bsp_payload.data(), bsp_payload.size());
        g_fn_node = (bsp_node*)(g_ldbuf + g_bufcap / 2);
        g_fn_base = (int32_t)(uintptr_t)g_fn_node;
        check("fixup_nodes", th_fixup_nodes, FP_PHYS);
    }
    regions_reset();
}

// ---- the BSP ------------------------------------------------------------------------------------------------------------
static uint8_t* g_bspbuf;
static uint32_t g_bsp_size;
static std::vector<bsp_tri*> g_btris;
static std::vector<bsp_node*> g_bnodes;
static const uint32_t TRI_BYTES = 0x60;                          // as the stock file lays them (0x5c used)
static void norm3(double* v) {
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= l; v[1] /= l; v[2] /= l;                             // (a degenerate triangle keeps its NaNs)
}
static void make_tri(bsp_tri* t, int kind) {
    double v[3][3];
    for (int k = 0; k < 3; k++) {
        if (k == 0 || kind == 1) { v[k][0] = rf(-100, 100); v[k][1] = rf(-40, 40); v[k][2] = rf(-100, 100); }
        else { v[k][0] = v[0][0] + rf(-30, 30); v[k][1] = v[0][1] + rf(-3, 3); v[k][2] = v[0][2] + rf(-30, 30); }
    }
    if (kind == 2) for (int c = 0; c < 3; c++) v[2][c] = v[0][c] + (v[1][c] - v[0][c]) * 0.5;   // collinear
    if (kind == 3) for (int c = 0; c < 3; c++) v[2][c] = v[1][c];                                 // two the same
    for (int k = 0; k < 3; k++) for (int c = 0; c < 3; c++) v[k][c] = (float)v[k][c];
    double e1[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
    double e2[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
    double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    norm3(n);
    if (n[1] < 0 && kind == 0) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
    memset(t, 0, sizeof *t);
    t->visited = 0;
    t->n = {(float)n[0], (float)n[1], (float)n[2]};
    t->d = (float)-(n[0] * v[0][0] + n[1] * v[0][1] + n[2] * v[0][2]);
    double cen[3];
    for (int c = 0; c < 3; c++) cen[c] = (v[0][c] + v[1][c] + v[2][c]) / 3;
    for (int k = 0; k < 3; k++) {
        t->v[k] = {(float)v[k][0], (float)v[k][1], (float)v[k][2]};
        const double* a = v[k];
        const double* b = v[(k + 1) % 3];
        double ed[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        double en[3] = {ed[1] * n[2] - ed[2] * n[1], ed[2] * n[0] - ed[0] * n[2], ed[0] * n[1] - ed[1] * n[0]};
        norm3(en);
        if ((cen[0] - a[0]) * en[0] + (cen[1] - a[1]) * en[1] + (cen[2] - a[2]) * en[2] > 0) { en[0] = -en[0]; en[1] = -en[1]; en[2] = -en[2]; }
        if (kind == 4) { en[0] = -en[0]; en[1] = -en[1]; en[2] = -en[2]; }   // inward: calc_sphere's "impossible"
        t->e[k] = {(float)en[0], (float)en[1], (float)en[2]};
    }
}
// a random tree over ntri triangles, in the file's form: node k at 12k (the root at 0), the triangles after them
static void make_bsp(std::vector<uint8_t>& out, int ntri) {
    const uint32_t tri0 = 12 * ntri;
    out.assign(tri0 + TRI_BYTES * ntri, 0);
    for (int j = 0; j < ntri; j++) {
        int kind = chance(65) ? 0 : chance(60) ? 1 : chance(40) ? 4 : chance(50) ? 2 : 3;
        make_tri((bsp_tri*)&out[tri0 + TRI_BYTES * j], kind);
    }
    for (int k = 0; k < ntri; k++) {
        uint32_t* nd = (uint32_t*)&out[12 * k];
        nd[0] = tri0 + TRI_BYTES * (chance(85) ? k : rnd() % ntri);
        if (k == 0) continue;
        for (;;) {                                               // hang it on a free slot of an earlier node
            uint32_t* p = (uint32_t*)&out[12 * (rnd() % k)];
            int side = 1 + rnd() % 2;
            if (!p[side]) { p[side] = 12 * k; break; }
        }
    }
}
static void load_bsp_payload(const std::vector<uint8_t>& payload) {
    // made usable by the ORIGINAL loader
    g_res[1] = {payload.data(), (uint32_t)payload.size(), (int32_t)payload.size(), 0, 1, 0, g_bspbuf, 0};
    ORIG(void(__cdecl*)(const char*), 0x46fb50)("track.bsp");
    g_bsp_size = (uint32_t)payload.size();
    g_btris.clear();
    g_bnodes.clear();
    bsp_node* head = G(bsp_node*, S_BSP_HEAD);
    std::vector<bsp_node*> st{head};
    while (!st.empty()) {
        bsp_node* n = st.back();
        st.pop_back();
        g_bnodes.push_back(n);
        bool seen = false;
        for (bsp_tri* t : g_btris) if (t == n->tri) seen = true;
        if (!seen) g_btris.push_back(n->tri);
        if (n->front) st.push_back(n->front);
        if (n->back) st.push_back(n->back);
    }
}
static bsp_tri* rbtri() { return g_btris[rnd() % g_btris.size()]; }
static P3 bsp_point(bool big) {
    uint32_t k = rnd() % 100;
    if (k < 50) {
        bsp_tri* t = rbtri();
        P3 p = on_tri3(t->v, rnd() % 5);
        float s = k_offsets[rnd() % (sizeof k_offsets / sizeof *k_offsets)];
        return {p.x + t->n.x * s, p.y + t->n.y * s, p.z + t->n.z * s};
    }
    if (k < 90) return big ? P3{rf(-2e5f, 2e5f), rf(-100, 100), rf(-2e5f, 2e5f)} : P3{rf(-120, 120), rf(-50, 50), rf(-120, 120)};
    P3 p = {rf(-120, 120), rf(-50, 50), rf(-120, 120)};
    (&p.x)[rnd() % 3] = k < 95 ? special() : (chance(50) ? 1 : -1) * rf(1e36f, 3.4e38f);
    return p;
}
static void th_bsp_hit(bool o) {
    Ar.ret = (uint32_t)(uintptr_t)(o ? ORIG(bsp_tri*(__cdecl*)(P3*, P3*, P3*), 0x46fbd0)(g_a0, g_a1, g_a2) : BSPHit(g_a0, g_a1, g_a2));
}
static void th_intersect_all(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__cdecl*)(P3*, P3*, bsp_node*), 0x46ff30)(g_a0, g_a1, g_fn_node) : intersect_all(g_a0, g_a1, g_fn_node);
}
static void th_intersect_tree(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__cdecl*)(P3*, P3*, bsp_node*), 0x46fc40)(g_a0, g_a1, g_fn_node) : intersect_tree(g_a0, g_a1, g_fn_node);
}
static bsp_tri* g_tri_arg;
static float* g_t_arg;
static void th_bsp_pip(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__cdecl*)(P3*, bsp_tri*), 0x470240)(g_a0, g_tri_arg) : bsp_point_in_poly(g_a0, g_tri_arg);
}
static void th_bsp_iplane(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__cdecl*)(P3*, P3*, bsp_tri*, P3*, float*), 0x4702e0)(g_a0, g_a1, g_tri_arg, g_a2, g_t_arg)
               : bsp_intersect_plane(g_a0, g_a1, g_tri_arg, g_a2, g_t_arg);
}
static void th_bsp_hit_sphere(bool o) {
    Ar.ret = (uint32_t)(uintptr_t)(o ? ORIG(bsp_tri*(__cdecl*)(P3*, float, P3*, P3*), 0x470450)(g_a0, Ar.f[0], g_a1, g_a2)
                                     : BSPHitSphere(g_a0, Ar.f[0], g_a1, g_a2));
}
static void th_sphere_tree(bool o) {
    Ar.ret = o ? ORIG(uint8_t(__cdecl*)(P3*, float, bsp_node*), 0x470560)(g_a0, Ar.f[0], g_fn_node)
               : sphere_intersect_tree(g_a0, Ar.f[0], g_fn_node);
}
static void th_calc_sphere(bool o) {
    if (o) ORIG(void(__cdecl*)(P3*, float, bsp_tri*), 0x470620)(g_a0, Ar.f[0], g_tri_arg);
    else calc_sphere_intersection(g_a0, Ar.f[0], g_tri_arg);
}
static void th_add_intersection(bool o) {
    if (o) ORIG(void(__cdecl*)(P3*, bsp_tri*, P3*, P3*), 0x470b40)(g_a0, g_tri_arg, g_a1, g_a2);
    else add_intersection(g_a0, g_tri_arg, g_a1, g_a2);
}
static void th_project(bool o) {
    if (o) ORIG(void(__cdecl*)(P3*, P3*, P3*), 0x470c70)(g_a0, g_a1, g_a2);
    else project_to_plane(g_a0, g_a1, g_a2);
}
static void bsp_state_random() {                                 // the statics a query starts from
    int32_t* c = (int32_t*)0x004f534c;
    for (int i = 0; i < 16; i++) c[i] = (int32_t)(rnd() % 1000);
    c[0] = (int32_t)(rnd() % 72);                                // the listed count (past 64: "possible overflow")
    bsp_tri** list = (bsp_tri**)(uintptr_t)S_BSP_LIST;
    for (int i = 0; i < 64; i++) list[i] = rbtri();
    for (bsp_tri* t : g_btris) t->visited = (uint8_t)(chance(20) ? 1 : 0);
    G(float, S_SPH_BEST_D2) = chance(50) ? rf(0, 400) : fbits(rnd());
    garbage((void*)(uintptr_t)S_SPH_BEST_P, 12);
    garbage((void*)(uintptr_t)S_SPH_BEST_N, 12);
    G(bsp_tri*, S_SPH_BEST_TRI) = (bsp_tri*)(uintptr_t)rnd();
    G(P3*, S_BSP_OUT) = &Ar.p[5];
    G(bsp_tri*, S_BSP_HIT) = (bsp_tri*)(uintptr_t)rnd();
}
static void bsp_queries(int nq, bool big) {
    regions_reset();
    regions_add(g_bspbuf, g_bsp_size, "bsp");
    for (int q = 0; q < nq; q++) {
        garbage(&Ar, sizeof Ar);
        bsp_state_random();
        FpMode fm = pick_fp();
        pick_args3();
        g_fn_node = g_bnodes[rnd() % g_bnodes.size()];
        if (chance(30)) g_fn_node = G(bsp_node*, S_BSP_HEAD);
        g_tri_arg = rbtri();
        Ar.p[0] = bsp_point(big);
        Ar.p[1] = chance(60) ? bsp_point(big) : P3{Ar.p[0].x + rf(-5, 5), Ar.p[0].y + rf(-5, 5), Ar.p[0].z + rf(-5, 5)};
        if (chance(10)) Ar.p[1] = Ar.p[0];
        float r = chance(5) ? special() : chance(50) ? rf(0.05f, 3) : rf(3, 60);
        Ar.f[0] = r;
        switch (q % 11) {
        case 0: check("BSPHit", th_bsp_hit, fm); break;
        case 1: check("intersect_all", th_intersect_all, fm); break;
        case 2: check("intersect_tree", th_intersect_tree, fm); break;
        case 3:
            if (chance(50)) *g_a0 = on_tri3(g_tri_arg->v, rnd() % 5);
            check("point_in_poly(bsp.obj)", th_bsp_pip, fm);
            break;
        case 4:
            g_t_arg = chance(20) ? 0 : &Ar.f[1];
            if (chance(20)) g_a2 = 0;
            check("intersect_plane(bsp.obj)", th_bsp_iplane, fm);
            break;
        case 5: check("BSPHitSphere", th_bsp_hit_sphere, fm); break;
        case 6: check("sphere_intersect_tree", th_sphere_tree, fm); break;
        case 7: case 8: {                                        // near a vertex, an edge or the face
            bsp_tri* t = g_tri_arg;
            P3 p = on_tri3(t->v, rnd() % 5);
            int k = rnd() % 3;
            float out = chance(50) ? rf(0, r) : rf(-r, r);
            P3 c = {p.x + t->e[k].x * out + t->n.x * rf(-r, r), p.y + t->e[k].y * out + t->n.y * rf(-r, r),
                    p.z + t->e[k].z * out + t->n.z * rf(-r, r)};
            *g_a0 = c;
            check("calc_sphere_intersection", th_calc_sphere, fm);
            break;
        }
        case 9:
            *g_a1 = bsp_point(big);
            *g_a2 = {rf(-1, 1), rf(-1, 1), rf(-1, 1)};
            check("add_intersection", th_add_intersection, fm);
            break;
        default:
            *g_a2 = g_tri_arg->e[rnd() % 3];
            check("project_to_plane", th_project, fm);
            break;
        }
    }
    regions_reset();
}

// ---- main ----------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int nq = argc > 1 ? atoi(argv[1]) : 100000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_wld_terrain.cpp");
    std::string game;
    if (s) { *s = 0; game = std::string(exe) + "\\..\\game-files\\installs\\v1.0-RC"; strcat(exe, "\\out\\race_v10.exe"); }
    if (argc > 3) game = argv[3];
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!load_race_exe(exe)) return 2;
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);              // __amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);              // __NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);              // __FF_MSGBANNER
    patch_jmp(0x004d6850, (void*)&stub_crt_fatal);              // __crtMessageBoxA
    for (int i = 0; i < g_nregs; i++) memcpy(g_regs[i].saved, (void*)(uintptr_t)g_regs[i].v10, 5);
    patch_jmp(0x00419fa0, (void*)&stub_resource_get);
    patch_jmp(0x0041a450, (void*)&stub_resource_forget);
    patch_jmp(0x00411150, (void*)&stub_log_report);
    patch_jmp(0x004112b0, (void*)&stub_log_panic);
    patch_jmp(0x004150a0, (void*)&stub_multi_begin);
    patch_jmp(0x00415220, (void*)&stub_multi_end);
    patch_jmp(0x00466870, (void*)&stub_view_set_sky);
    patch_jmp(0x0046dfb0, (void*)&stub_graf_min_lod);
    patch_jmp(0x0046db30, (void*)&stub_graf_load);
    patch_jmp(0x0046dc20, (void*)&stub_graf_unload);
    printf("%d rewrites registered\n", g_nregs);
    g_bppbuf = (uint8_t*)VirtualAlloc(0, g_bufcap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_ldbuf = (uint8_t*)VirtualAlloc(0, g_bufcap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_bspbuf = (uint8_t*)VirtualAlloc(0, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    // the tracks
    std::vector<std::string> files;
    WIN32_FIND_DATAA fd;
    const char* pats[] = {"\\*.trk", "\\Backups\\*_original.trk.bak"};
    for (const char* pat : pats) {
        HANDLE h = FindFirstFileA((game + pat).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::string dir = game + (strstr(pat, "Backups") ? "\\Backups\\" : "\\");
        do files.push_back(dir + fd.cFileName); while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (files.empty()) { printf("no tracks under %s\n", game.c_str()); return 2; }
    regions_reset();
    std::vector<uint8_t> real_bsp;
    for (int chain = 0; chain < 2; chain++) {
        g_chain = chain != 0;
        printf("---- %s ----\n", g_chain ? "chain: every original entry jumps to its rewrite" : "isolated: each rewrite, its callees original");
        for (const std::string& path : files) {
            Track t;
            if (!read_track(path, t)) { printf("  %s: no track.bpp / track.bsp\n", path.c_str()); continue; }
            if (t.bpp.size() > g_bufcap / 2) { printf("  %s: too big\n", path.c_str()); continue; }
            if (real_bsp.empty()) real_bsp = t.bsp;
            // the live tree, made by the original BPPLoad
            g_res[0] = {t.bpp.data(), (uint32_t)t.bpp.size(), (int32_t)t.bpp.size(), t.bpp_ver, 1, 0, g_bppbuf, g_bufcap};
            ORIG(void(__cdecl*)(const char*), 0x46d070)("track.bpp");
            g_hdr = G(bpp_header*, S_BPP_HEADER);
            g_box = {1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f};
            for (int i = 0; i < g_hdr->n_tris; i++)
                for (int k = 0; k < 3; k++) {
                    const P3& v = g_hdr->tris[i].v[k];
                    g_box[0] = std::min(g_box[0], v.x); g_box[1] = std::max(g_box[1], v.x);
                    g_box[2] = std::min(g_box[2], v.z); g_box[3] = std::max(g_box[3], v.z);
                    g_box[4] = std::min(g_box[4], v.y); g_box[5] = std::max(g_box[5], v.y);
                }
            std::vector<uint8_t> tree_before(g_bppbuf, g_bppbuf + t.bpp.size());
            long bad0 = 0;
            for (int i = 0; i < g_ntally; i++) bad0 += g_tally[i].bad;
            bpp_queries(nq);
            bool tree_same = !memcmp(tree_before.data(), g_bppbuf, t.bpp.size());
            long bad1 = 0;
            for (int i = 0; i < g_ntally; i++) bad1 += g_tally[i].bad;
            const char* nm = strrchr(path.c_str(), '\\');
            printf("  %-28s %6d triangles %6d nodes: %d queries, %s%s\n", nm ? nm + 1 : path.c_str(), g_hdr->n_tris, g_hdr->n_nodes,
                   nq, bad1 == bad0 ? "identical" : "DIFFER", tree_same ? "" : " (THE TREE CHANGED)");
            if (!tree_same) g_tally[0].bad++;
            loader_tests(t, t.bsp, t.bsp_ver);
            G(bpp_header*, S_BPP_HEADER) = g_hdr;
        }
        // the BSP: the stock one, then random ones
        load_bsp_payload(real_bsp);
        bsp_queries(nq / 4, true);
        for (int w = 0; w < 40; w++) {
            std::vector<uint8_t> payload;
            make_bsp(payload, 1 + rnd() % (w < 10 ? 8 : 150));
            load_bsp_payload(payload);
            bsp_queries(nq / 20, false);
            if (w < 6) {                                         // the loaders on a synthetic BSP
                g_res[0] = {0, 0, 0, 2, 1, 1, 0, 0};
                g_res[1] = {payload.data(), (uint32_t)payload.size(), (int32_t)payload.size(), 0, 1, 0, g_ldbuf + g_bufcap / 2, 0};
                regions_reset();
                regions_add(g_ldbuf + g_bufcap / 2, (uint32_t)payload.size(), "bsp buffer");
                for (int v = 0; v < 4; v++) {
                    garbage(&Ar, sizeof Ar);
                    strcpy(Ar.name, "track.bsp");
                    g_res[1].fresh = v != 1;
                    g_res[1].version = v == 2 ? 5 : 0;
                    g_res[1].fail = v == 3;
                    if (v == 1) memcpy(g_ldbuf + g_bufcap / 2, payload.data(), payload.size());
                    check("BSPLoad", th_bsp_load, FP_PHYS);
                    check("load_file(bsp.obj)", th_bsp_load_file, FP_PHYS);
                    memcpy(g_ldbuf + g_bufcap / 2, payload.data(), payload.size());
                    g_fn_node = (bsp_node*)(g_ldbuf + g_bufcap / 2);
                    g_fn_base = (int32_t)(uintptr_t)g_fn_node;
                    if (v == 0) ((uint32_t*)g_fn_node)[0] = 0;   // a null triangle: "bad node"
                    check("fixup_nodes", th_fixup_nodes, FP_PHYS);
                }
                regions_reset();
                load_bsp_payload(payload);
            }
        }
        G(bsp_node*, S_BSP_HEAD) = 0;
    }
    int failed = 0;
    printf("---- per function (both rounds) ----\n");
    for (int i = 0; i < g_ntally; i++) {
        const Tally& t = g_tally[i];
        printf("  %-44s %9ld checks %8ld returned nonzero %6ld faulted: %s\n", t.name, t.n, t.hits, t.faults, t.bad ? "DIFFERS" : "identical");
        failed += t.bad != 0;
    }
    printf("%s\n", failed ? "FAILED" : "all identical");
    return failed ? 1 : 0;
}
