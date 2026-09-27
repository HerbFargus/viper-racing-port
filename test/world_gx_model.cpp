// world_gx_model.cpp -- group G1b's rewrites (hook/gx_model.cpp: mr.obj, dxmatrix.obj, feature.obj, light.obj,
// model.obj) against the originals, on random worlds, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_gx_model.cpp
//        /Fo<dir>\ /Fe<dir>\world_gx_model.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_gx_model.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. Every world
// starts from the loaded image's .data, then the game's own set-up run by the ORIGINAL code: mr_model_begin (the
// pools -- M1's pool-size operands patched down in the image, so the pools stay small and the rewrites' m1_operand
// reads are exercised -- the lit-vertex buffer, begin_deferred), mr_feature_begin, light_begin, init_camera,
// mrLightMode, then 1..4 random models (mrModelCreate + mrModelBuild / BuildLit on random mrModelInfos: surfaces of
// every type, vertex and triangle ranges, names including 16-character ones, remap tables), sometimes a copy
// (mrModelCopyTransform) and some surfaces queued in the deferred buckets. Then the statics are randomised: the view,
// projection and camera, the dxState block and the feature stack, the lighting (sun, camera and half vector in model
// space, fog, the eight lights, env mode, alpha, the tables' function picks), the clip distances and model flags.
//
// Stubbed (a jump to a logger), everything below this group: MemAlloc (a bump heap in the arena), MemFree,
// operator delete, LogPanic, LogReport, ResourceGet (a random .mod resource image, fresh or not, of either version)
// / ResourceForget / ResourceWrite, FileCreate / FileWrite / FileClose, the dx layer (dxBegin / dxEnd / dxBegin3D /
// dxEnd3D / dxRelease / dxRestore / dxGetCounters / dxSetViewport / dxSetViewportColor / dxClearZ / dxProjectPoint --
// a deterministic fake projection that writes its output and sometimes fails -- the frame, state, flush, begin,
// draw and end calls, logging the vertex and index data they are handed, and d3d::SetTransform, logging the matrix),
// the texture layer (TextureBegin / End / ReleaseAll / RestoreAll / BeginFrame / EndFrame / Flush /
// WillDeresNextFrame / HasAlpha, gxGetTexture / gxForgetTexture), model.obj's ASSERT_MSG (logging its doubles) and
// the profiler's four function pointers. Everything else runs as the game's code: PoolBase, the CRT's strncpy /
// stricmp / __ftol, the physics library's MatrixConcat / MatrixMulPointInv / VectorSub / VectorLength /
// _VectorNormalize, and this group's own functions where one calls another (both passes call them by address).
//
// Each world picks a function (or a sequence: the deferred surfaces queued and drawn, stock -- with texture numbers
// past the 120-entry array, which both overrun alike -- and lifted: begin_deferred's operands repointed at a 1,024-bucket
// array as M1 does, the original running the stock code on the stock array, the rewrite the lifted one, which must
// draw exactly the same for texture numbers below 118); the arena and the image's whole .data/.bss are saved, the
// original runs, the result is kept, the world restored, the rewrite runs; the arena, .data/.bss, the return value
// and the stubs' logs are compared, and every byte the original changed must lie in the rewrite's footprint unless
// it's replay_only (all of it runs on the main thread: no statics are exempt). The FPU runs at a random precision
// (24 or 53 bits), in 30% of the worlds with overflow and divide-by-zero unmasked; a share of the worlds has floats
// replaced by extreme values (never the pointers). Nothing modal: the CRT's fatal paths are stubbed.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#define VP_GX_HARNESS               // gx_model.cpp: the deferred buckets' M1 bookkeeping stays local
#include "../hook/port.h"
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { printf("footprint: object() not expected (%s)\n", what); add(obj, 4, what); }
void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
uint32_t A(uint32_t v10) { return v10; }
bool build_is_v10() { return true; }

#include "../hook/gx_model.cpp"

// ---- random numbers --------------------------------------------------------------------------------------
static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float range(float a, float b) { return a + (b - a) * uni(); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits_h(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
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
static void rnd_unit(void* p) {
    float* f = (float*)p;
    double x = range(-1, 1), y = range(-1, 1), z = range(-1, 1), l = sqrt(x * x + y * y + z * z);
    if (l < 1e-3) { x = 0; y = 1; z = 0; l = 1; }
    const double s = chance(90) ? 1.0 : range(0.5f, 1.6f);
    f[0] = (float)(x / l * s); f[1] = (float)(y / l * s); f[2] = (float)(z / l * s);
}
static void random_rot(float* m) {                                   // a rotation (roughly: sometimes skewed)
    const double a = range(0, 6.2831853f), b = range(-1.5f, 1.5f), c = range(0, 6.2831853f);
    const double ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b), cc = cos(c), sc = sin(c);
    const double r[9] = {ca * cc - sa * sb * sc, -cb * sc, sa * cc + ca * sb * sc,
                         ca * sc + sa * sb * cc, cb * cc, sa * sc - ca * sb * cc,
                         -sa * cb, sb, ca * cb};
    for (int i = 0; i < 9; i++) m[i] = (float)r[i];
    if (chance(5)) for (int i = 0; i < 9; i++) m[i] = range(-1.2f, 1.2f);
}
static void random_frame(float* f, float spread) { random_rot(f); rnd_p3(f + 9, -spread, spread); }
static uint32_t hash_bytes(const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
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

// ---- the world: an arena, and the image's .data/.bss ------------------------------------------------------------
enum : uint32_t {
    ARENA_BYTES = 0x60000,
    A_HEAP = 0x00000, A_HEAP_END = 0x30000,  // MemAlloc's bump heap: the pools, the lit buffer, index arrays, copies
    A_INFO = 0x30000,                        // four mrModelInfos, 0x4000 each
    A_RES = 0x40000,                         // a .mod resource image (0x4000)
    A_OUT = 0x44000,                         // an output vertex buffer (1500 x 32)
    A_ARGS = 0x50000,                        // arguments
    A_LIFT = 0x58000,                        // M1's lifted bucket array (1024 x 4)
};
enum : uint32_t {
    G_FRAME = 0x000, G_P3A = 0x040, G_P3B = 0x050, G_P3C = 0x060, G_OUT = 0x080, G_MAT = 0x0c0, G_NAME = 0x100,
    G_NAME2 = 0x140, G_INTS = 0x180, G_REMAP = 0x200, G_REMAP_OUT = 0x380, G_TRIS = 0x400, G_IDX = 0x1400, G_SURF = 0x1800,
};
static uint8_t* g_arena;
static uint8_t* g_arena_snap;
static uint8_t* g_arena_after;
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* g_data_pristine;
static uint8_t* g_data_snap;
static uint8_t* g_data_after;
static uint8_t* AR(uint32_t off) { return g_arena + off; }
static uint8_t* ARG(uint32_t off) { return g_arena + A_ARGS + off; }
static bool in_arena(const void* p, uint32_t n) { return (const uint8_t*)p >= g_arena && (const uint8_t*)p + n <= g_arena + ARENA_BYTES && n < ARENA_BYTES; }
static uint32_t rel(const void* p) {                                  // a pointer as the log sees it
    if (in_arena(p, 1)) return 0x10000000u + (uint32_t)((const uint8_t*)p - g_arena);
    if ((uintptr_t)p >= 0x400000 && (uintptr_t)p < 0x700000) return (uint32_t)(uintptr_t)p;
    return p ? 0xdead0000u : 0;                                      // a stack local: its contents are logged instead
}

// ---- the stubs' log and script ------------------------------------------------------------------------------------
enum { LOG_MAX = 32768 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void log_word(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static void log_str(const char* s) {
    if (!s) { log_word(0xffffffff); return; }
    size_t n = strnlen(s, 64);
    log_word((uint32_t)n);
    log_word(hash_bytes(s, (uint32_t)n));
}
static void log_block(const void* p, uint32_t n) {                   // where, and a hash of what's there
    log_word(rel(p));
    log_word(n);
    if (p && n && n < 0x100000 && (in_arena(p, n) || rel(p) == 0xdead0000u || ((uintptr_t)p >= 0x400000 && (uintptr_t)p < 0x700000)))
        log_word(hash_bytes(p, n));
}

struct Script {
    uint32_t salt;                  // TextureHasAlpha, dxProjectPoint
    uint8_t dx_ok, tex_ok, res_found, res_fresh, file_ok;
    uint32_t res_version;
    int deres_frames;
    int proj_fail_pct;
};
static Script g_script;
static uint8_t* g_heap;             // the bump pointer
static uint8_t* g_heap_mark;
static int g_tex_next, g_deres_left, g_file_next;

static void* __cdecl stub_MemAlloc(int n) {
    log_word('MALC'); log_word((uint32_t)n);
    if (n < 0 || (uint32_t)n > (uint32_t)(AR(A_HEAP_END) - g_heap)) { log_word(1); return 0; }
    uint8_t* p = g_heap;
    g_heap += ((uint32_t)n + 7) & ~7u;
    log_word(rel(p));
    return p;
}
static void __cdecl stub_MemFree(void* p) { log_word('MFRE'); log_word(rel(p)); }
static void __cdecl stub_delete(void* p) { log_word('DELE'); log_word(rel(p)); }
static void __cdecl stub_panic(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    log_word('PANC'); log_word((uint32_t)(uintptr_t)fmt);
    if ((uint32_t)(uintptr_t)fmt == 0x004f227c) log_str(va_arg(ap, const char*));
    if ((uint32_t)(uintptr_t)fmt == 0x004f2230) { log_word(va_arg(ap, uint32_t)); log_word(va_arg(ap, uint32_t)); }
    va_end(ap);
}
static void __cdecl stub_report(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    log_word('REPT'); log_word((uint32_t)(uintptr_t)fmt);
    if ((uint32_t)(uintptr_t)fmt == 0x004f2258) log_str(va_arg(ap, const char*));
    va_end(ap);
}
static void* __cdecl stub_ResourceGet(const char* name, uint32_t type, uint32_t* ver, int* size, uint8_t* x, uint8_t* fresh) {
    log_word('RGET'); log_str(name); log_word(type); log_word((uint32_t)(uintptr_t)x);
    if (!g_script.res_found) return 0;
    *ver = g_script.res_version;
    *size = 0x4000;
    *fresh = g_script.res_fresh;
    return AR(A_RES);
}
static uint8_t __cdecl stub_ResourceForget(void* p) { log_word('RFGT'); log_word(rel(p)); return 1; }
static void __cdecl stub_ResourceWrite(int h, uint32_t type, uint32_t v) { log_word('RWRT'); log_word((uint32_t)h); log_word(type); log_word(v); }
static int __cdecl stub_FileCreate(const char* name) { log_word('FCRE'); log_str(name); return g_script.file_ok ? 0x40 + g_file_next++ : 0; }
static uint8_t __cdecl stub_FileWrite(int h, const void* p, int n) { log_word('FWRT'); log_word((uint32_t)h); log_block(p, (uint32_t)n); return 1; }
static void __cdecl stub_FileClose(int* h) { log_word('FCLS'); log_word((uint32_t)*h); *h = 0; }
// the dx layer
static uint8_t __cdecl stub_dxBegin() { log_word('DXBG'); return g_script.dx_ok; }
static void __cdecl stub_dxEnd() { log_word('DXEN'); }
static void __cdecl stub_dxGetCounters(int* a, int* b, int* c, int* d) {
    log_word('DXGC'); log_word(rel(a)); log_word(rel(b)); log_word(rel(c)); log_word(rel(d));
    *a = 1; *b = 22; *c = 333; *d = 4444;
}
static uint8_t __cdecl stub_dxBegin3D(void* caps) { log_word('DX3B'); log_word(rel(caps)); return g_script.dx_ok; }
static void __cdecl stub_dxEnd3D() { log_word('DX3E'); }
static void __cdecl stub_dxRelease() { log_word('DXRL'); }
static void __cdecl stub_dxRestore() { log_word('DXRS'); }
static uint8_t __cdecl stub_dxSetViewport(int x, int y, int w, int h) { log_word('DXVP'); log_word(x); log_word(y); log_word(w); log_word(h); return 1; }
static void __cdecl stub_dxSetViewportColor(uint32_t r, uint32_t g, uint32_t b) { log_word('DXVC'); log_word(r); log_word(g); log_word(b); }
static void __cdecl stub_dxClearZ(int a, int b, int c, int d, uint8_t t) { log_word('DXCZ'); log_word(a); log_word(b); log_word(c); log_word(d); log_word(t); }
static uint8_t __cdecl stub_dxProjectPoint(const float* in, float* out, uint8_t clip) {
    log_word('DXPP'); log_block(in, 12); log_word(clip);
    const uint32_t h = hash_bytes(in, 12) ^ g_script.salt;
    const float s = 1.0f + (float)(h % 7);
    out[0] = in[0] * s + 320.0f;
    out[1] = 240.0f - in[1] * s;
    out[2] = (float)(h % 1000) / 1000.0f;
    return (int)(h % 100) >= g_script.proj_fail_pct ? 1 : 0;
}
static void __cdecl stub_dxDPBeginFrame() { log_word('DXBF'); }
static void __cdecl stub_dxDPEndFrame() { log_word('DXEF'); }
static void __cdecl stub_dxEnableVertexAlpha(uint8_t on) { log_word('DXVA'); log_word(on); }
static void __cdecl stub_dxDPBegin(int tex) { log_word('DXDB'); log_word((uint32_t)tex); }
static void __cdecl stub_dxDPDraw(const void* v, int nv, const uint16_t* idx, int ni) {
    log_word('DXDD'); log_block(v, nv > 0 && nv < 4096 ? 32u * nv : 0); log_word(nv); log_block(idx, ni > 0 && ni < 20000 ? 2u * ni : 0); log_word(ni);
}
static void __cdecl stub_dxDPEnd() { log_word('DXDE'); }
static void __cdecl stub_dxBeginTriangles(int tex) { log_word('DXBT'); log_word((uint32_t)tex); }
static void __cdecl stub_dxDrawTriangle(const void* v) { log_word('DXD1'); log_block(v, 96); }
static void __cdecl stub_dxDrawTriangles(const void* v, int n) { log_word('DXDN'); log_block(v, n > 0 && n < 4096 ? 32u * n : 0); log_word(n); }
static void __cdecl stub_dxDrawIndexTriangles(const void* v, int n, const uint16_t* idx, int ni) {
    log_word('DXDI'); log_block(v, n > 0 && n < 4096 ? 32u * n : 0); log_word(n); log_block(idx, ni > 0 && ni < 20000 ? 2u * ni : 0); log_word(ni);
}
static void __cdecl stub_dxDrawScreen(const void* v, int n, const uint16_t* idx, int ni) {
    log_word('DXDS'); log_block(v, n > 0 && n < 64 ? 32u * n : 0); log_word(n); log_block(idx, ni > 0 && ni < 64 ? 2u * ni : 0); log_word(ni);
}
static void __cdecl stub_dxEndTriangles() { log_word('DXET'); }
static void __cdecl stub_dxStateInit() { log_word('DXSI'); }
static void __cdecl stub_dxStateClear() { log_word('DXSC'); }
static void __cdecl stub_dxStateUpdate(void* s) { log_word('DXSU'); log_word(rel(s)); log_word(hash_bytes((void*)0x005229d4, 0x1c)); }
static void __cdecl stub_dxStateFlush() { log_word('DXSF'); log_word(hash_bytes((void*)0x005229d4, 0x1c)); }
static void __fastcall stub_SetTransform(void* self, int, int type, void* m) { log_word('D3ST'); log_word(rel(self)); log_word((uint32_t)type); log_block(m, 64); }
// the texture layer
static uint8_t __cdecl stub_TextureBegin(int a, int b, int c) { log_word('TXBG'); log_word(a); log_word(b); log_word(c); return g_script.tex_ok; }
static void __cdecl stub_TextureEnd() { log_word('TXEN'); }
static void __cdecl stub_TextureReleaseAll(uint8_t b) { log_word('TXRL'); log_word(b); }
static void __cdecl stub_TextureRestoreAll(uint8_t b) { log_word('TXRS'); log_word(b); }
static uint8_t __cdecl stub_TextureWillDeres() { log_word('TXWD'); return g_deres_left-- > 0 ? 1 : 0; }
static void __cdecl stub_TextureBeginFrame() { log_word('TXBF'); }
static void __cdecl stub_TextureEndFrame() { log_word('TXEF'); }
static void __cdecl stub_TextureFlush() { log_word('TXFL'); }
static uint8_t __cdecl stub_TextureHasAlpha(int id) { log_word('TXHA'); log_word((uint32_t)id); return (((uint32_t)id * 0x9e3779b1u ^ g_script.salt) >> 7) & 1; }
static int __cdecl stub_gxGetTexture(const char* name, int v) { log_word('GXGT'); log_str(name); log_word((uint32_t)v); return g_tex_next++; }
static void __cdecl stub_gxForgetTexture(int t) { log_word('GXFG'); log_word((uint32_t)t); }
static void __cdecl stub_ASSERT_MSG(int ok, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    log_word('ASRT'); log_word((uint32_t)ok); log_word((uint32_t)(uintptr_t)fmt);
    if ((uint32_t)(uintptr_t)fmt == 0x004f2204)
        for (int k = 0; k < 3; k++) { double d = va_arg(ap, double); uint64_t b; memcpy(&b, &d, 8); log_word((uint32_t)b); log_word((uint32_t)(b >> 32)); }
    va_end(ap);
}
static void __cdecl stub_prof_ob() { log_word('POB'); }
static void __cdecl stub_prof_oe() { log_word('POE'); }
static int __cdecl stub_prof_start(const char* name) { log_word('PST'); log_word((uint32_t)(uintptr_t)name); return 7; }
static void __cdecl stub_prof_stop(int h) { log_word('PSP'); log_word((uint32_t)h); }

// the game's CRT must never put up a modal box: its fatal paths end the test quietly
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }
static int __cdecl stub_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}

// ---- building the world -----------------------------------------------------------------------------------------
typedef void(__cdecl* V0_t)();
static int g_models[8];
static int g_nmodels;
static uint8_t* info_at(int k) { return AR(A_INFO + 0x4000 * (uint32_t)k); }
static int g_ninfo = 4;
static const char* const k_names[] = {"tex_a", "tex_b", "wheel", "glass", "", "sixteen_chars_ab", "TEX_A", "envmap"};

// a random mrModelInfo at p (contiguous, like a loaded .mod): vertices, surfaces, triangles, the two record arrays
static void random_info(uint8_t* p, int budget_verts) {
    memset(p, 0, 0x4000);
    const int ns = chance(5) ? 0 : 1 + (int)(rnd() % 6);
    int nv = 0, nt = 0;
    int16_t vr[6][2], tr[6][2];
    for (int s = 0; s < ns; s++) {
        const int v = chance(5) ? 0 : 3 + (int)(rnd() % 40), t = chance(5) ? 0 : 1 + (int)(rnd() % 30);
        if (nv + v > budget_verts || nv + v > 200) { vr[s][0] = vr[s][1] = (int16_t)nv; }
        else { vr[s][0] = (int16_t)nv; vr[s][1] = (int16_t)(nv + v); nv += v; }
        if (nt + t > 200) { tr[s][0] = tr[s][1] = (int16_t)nt; } else { tr[s][0] = (int16_t)nt; tr[s][1] = (int16_t)(nt + t); nt += t; }
    }
    const int n18 = (int)(rnd() % 4), n20 = (int)(rnd() % 5);
    int32_t* h = (int32_t*)p;
    uint8_t* q = p + 0x28;
    h[0] = nv; h[1] = (int32_t)(uintptr_t)q; float* verts = (float*)q; q += 32 * nv;
    h[2] = ns; h[3] = (int32_t)(uintptr_t)q; uint8_t* surfs = q; q += 32 * ns;
    h[4] = nt; h[5] = (int32_t)(uintptr_t)q; int16_t* tris = (int16_t*)q; q += 8 * nt;
    h[6] = n18; h[7] = (int32_t)(uintptr_t)q; for (int i = 0; i < 16 * n18; i++) q[i] = (uint8_t)rnd(); q += 16 * n18;
    h[8] = n20; h[9] = (int32_t)(uintptr_t)q; for (int i = 0; i < 4 * n20; i++) q[i] = (uint8_t)rnd(); q += 4 * n20;
    if (!nv) h[1] = chance(50) ? 0 : h[1];
    for (int i = 0; i < nv; i++) {
        float* v = verts + 8 * i;
        rnd_p3(v, -40.0f, 40.0f);
        rnd_unit(v + 3);
        v[6] = range(-1, 2); v[7] = range(-1, 2);
        if (rnd() % 1000 < 3) wild_at(&v[rnd() % 8]);
    }
    for (int s = 0; s < ns; s++) {
        uint8_t* sf = surfs + 32 * s;
        strncpy((char*)sf, k_names[rnd() % 8], 16);
        const int kk = rnd() % 20;
        sf[0x10] = (uint8_t)(kk < 8 ? 0 : kk < 15 ? 1 : kk < 19 ? 2 : 3 + rnd() % 200);
        sf[0x11] = (uint8_t)(rnd() % 3);
        *(uint16_t*)(sf + 0x14) = (uint16_t)rnd(); *(uint16_t*)(sf + 0x16) = (uint16_t)rnd();
        *(int16_t*)(sf + 0x18) = vr[s][0]; *(int16_t*)(sf + 0x1a) = vr[s][1];
        *(int16_t*)(sf + 0x1c) = tr[s][0]; *(int16_t*)(sf + 0x1e) = tr[s][1];
        for (int t = tr[s][0]; t < tr[s][1]; t++) {
            const int span = vr[s][1] - vr[s][0];
            for (int c = 0; c < 3; c++) tris[4 * t + c] = (int16_t)(span > 0 ? vr[s][0] + (int)(rnd() % span) : rnd() % (nv ? nv : 1));
            tris[4 * t + 3] = (int16_t)rnd();
        }
    }
}
// the .mod resource image: its pointers are left as garbage (a fresh load makes them)
static void random_resource() {
    uint8_t* r = AR(A_RES);
    random_info(r, 200);
    for (int k = 0; k < 5; k++) *(uint32_t*)(r + 4 + 8 * k) = chance(50) ? rnd() : 0;
    if (chance(5)) *(int32_t*)(r + 4 * (rnd() % 5) * 2) = -(int32_t)(rnd() % 3);
}

static bool g_setup_ok;
static int setup_filter(EXCEPTION_POINTERS* e) {
    if (getenv("VP_SETUP")) {
        printf("  setup fault %08lx at %08lx; ecx %08lx edx %08lx; stack:", e->ExceptionRecord->ExceptionCode, (unsigned long)e->ContextRecord->Eip,
               (unsigned long)e->ContextRecord->Ecx, (unsigned long)e->ContextRecord->Edx);
        const uint32_t* sp = (const uint32_t*)(uintptr_t)e->ContextRecord->Esp;
        for (int q = 0; q < 64; q++) if (sp[q] >= 0x401000 && sp[q] < 0x4e0000) printf(" %08x", sp[q]);
        printf("\n");
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
static void setup_world() {
    memcpy(DATA, g_data_pristine, DATA_BYTES);
    memset(g_arena, 0, ARENA_BYTES);
    memset(AR(A_HEAP), 0xcd, A_HEAP_END - A_HEAP);
    g_heap = AR(A_HEAP);
    g_tex_next = 1;
    g_file_next = 0;
    g_nmodels = 0;
    g_script.salt = rnd();
    unsigned cw;
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    g_setup_ok = true;
    int budget = 1500;
    for (int k = 0; k < g_ninfo; k++) {
        random_info(info_at(k), budget);
        budget -= *(int32_t*)info_at(k);
    }
    random_resource();
    __try {
        ((V0_t)0x004556f0)();                                        // mr_model_begin
        ((V0_t)0x00457860)();                                        // mr_feature_begin
        ((V0_t)0x0045ae10)();                                        // light_begin
        ((V0_t)0x0044f520)();                                        // init_camera
        ((V0_t)0x0044f490)();                                        // init_render_state
        ((void(__cdecl*)(int))0x0045bf70)(chance(60) ? 0 : 1);       // mrLightMode
        const int nm = 1 + (int)(rnd() % 4);
        for (int k = 0; k < nm; k++) {
            uint8_t* info = info_at(k);
            const int m = ((int(__cdecl*)(const char*, int))0x00455b40)(k_names[rnd() % 8], *(int32_t*)(info + 0x10));
            if (chance(30)) {                                        // a remap table on it
                uint8_t* rm = ARG(G_REMAP) + 0x28 * 4 * (k % 1);
                const int n = (int)(rnd() % 4);
                for (int e = 0; e < n; e++) {
                    strncpy((char*)rm + 0x28 * e, k_names[rnd() % 8], 16);
                    strncpy((char*)rm + 0x28 * e + 0x10, k_names[rnd() % 8], 16);
                    *(int32_t*)(rm + 0x28 * e + 0x20) = (int32_t)(rnd() % 3);
                    *(uint8_t**)(rm + 0x28 * e + 0x24) = chance(70) ? ARG(G_REMAP_OUT) + 4 * e : 0;
                }
                *(uint8_t**)((uint8_t*)(uintptr_t)m + 0x18) = rm;
                *(int32_t*)((uint8_t*)(uintptr_t)m + 0x1c) = n;
            }
            ((void(__cdecl*)(int, void*, int))(chance(50) ? 0x00455c80 : 0x00455ca0))(m, info, 7);
            g_models[g_nmodels++] = m;
        }
        if (chance(30) && g_nmodels) {
            float fr[12];
            random_frame(fr, 20.0f);
            g_models[g_nmodels++] = ((int(__cdecl*)(int, const void*))0x004559c0)(g_models[rnd() % g_nmodels], fr);
        }
        if (chance(40)) {                                            // surfaces queued in the (stock) buckets
            ((V0_t)0x00457280)();
            const int n = (int)(rnd() % 12);
            for (int k = 0; k < n && g_nmodels; k++) {
                uint8_t* mi = (uint8_t*)(uintptr_t)g_models[rnd() % g_nmodels];
                uint8_t* info = *(uint8_t**)(mi + 0x10);
                uint8_t* si = *(uint8_t**)(mi + 0x28);
                if (!si || !info || *(int32_t*)(info + 8) <= 0) continue;
                if (chance(50)) *(int32_t*)(si + 0x14) = (int32_t)(rnd() % 116);
                ((void(__cdecl*)(void*, void*, void*))0x004573d0)(info, *(uint8_t**)(info + 0xc), si);
            }
        }
    } __except (setup_filter(GetExceptionInformation())) { g_setup_ok = false; }
    _controlfp_s(&cw, _PC_53, _MCW_PC);
}

static void random_statics() {
    // mr.obj
    int32_t* v = (int32_t*)0x005229a0;
    for (int i = 0; i < 8; i++) v[i] = (int32_t)(rnd() % 1200) - 100;
    *(float*)S_NEAR = chance(80) ? range(0.1f, 2.0f) : range(-1, 5);
    *(float*)S_FAR = chance(80) ? range(100.0f, 3000.0f) : range(-10, 10);
    *(float*)S_FOV = range(0.2f, 2.5f);
    for (int i = 0; i < 16; i++) ((float*)S_PROJ)[i] = range(-2, 2);
    for (int i = 0; i < 16; i++) ((float*)S_CAMMAT)[i] = range(-2, 2);
    rnd_unit((void*)S_CAMDIR);
    rnd_p3((void*)S_CAMPOS, -200, 200);
    *(uint32_t*)S_BGCOLOR = rnd();
    *(int32_t*)S_ALPHA_LEVEL = (int32_t)(rnd() % 4);
    *(int32_t*)S_SCREEN_WID = 320 + (int32_t)(rnd() % 1400); *(int32_t*)S_SCREEN_HIT = 200 + (int32_t)(rnd() % 1000);
    *(int32_t*)S_GX_WID = 320 + (int32_t)(rnd() % 1400); *(int32_t*)S_GX_HIT = 200 + (int32_t)(rnd() % 1000);
    uint8_t* s = (uint8_t*)S_FEAT2;
    for (int i = 0; i < 0x1c; i++) s[i] = (uint8_t)(chance(90) ? rnd() % 2 : rnd());
    *(uint32_t*)(S_DXSTATE + 4) = rnd();
    *(int32_t*)(S_DXSTATE + 0x10) = (int32_t)(rnd() % 3);
    *(uint8_t*)S_MR_OK = (uint8_t)(rnd() % 3);
    *(uint8_t*)S_MR_ON = (uint8_t)(chance(80) ? 1 : 0);
    *(int32_t*)S_FSTACK_IDX = (int32_t)(rnd() % 4);
    for (int i = 0; i < 6; i++) {
        ((uint32_t*)S_FSTACK)[2 * i] = chance(70) ? rnd() & 0xfff : rnd();
        static const int modes[] = {1, 2, 4, 3, 0};
        ((int32_t*)S_FSTACK)[2 * i + 1] = modes[rnd() % 5];
    }
    // light.obj
    *(uint8_t*)L_ON = (uint8_t)(chance(85) ? 1 : 0);
    *(int32_t*)L_MODE = chance(50) ? 0 : chance(90) ? 1 : 2;
    rnd_p3((void*)L_CAMPOS, -300, 300);
    rnd_unit((void*)L_CAMDIR);
    if (chance(20)) rnd_unit((void*)L_SUN);
    *(int32_t*)L_BASE = chance(90) ? 0x80 : (int32_t)(rnd() % 0x100);
    rnd_unit((void*)L_MSUN);
    for (int i = 0; i < 3; i++) ((float*)L_MSUN)[i] *= 255.0f;
    rnd_p3((void*)L_MCAM, -300, 300);
    if (chance(20)) rnd_p3((void*)L_MCAM, -1500, 1500);
    rnd_unit((void*)L_MCAMDIR);
    rnd_unit((void*)L_HALF);
    const float a = chance(70) ? 1.0f : range(0, 1);
    *(float*)L_ALPHA = a;
    *(uint8_t*)L_ALPHA_B = (uint8_t)(a * 255);
    *(uint32_t*)L_ALPHA_OR = (uint32_t)(uint8_t)(a * 255) << 24;
    *(uint8_t*)L_SPEC = (uint8_t)chance(50);
    *(uint8_t*)L_FOG = (uint8_t)chance(60);
    if (chance(50)) { *(uint32_t*)L_FOG_START = 0x47afc800; *(uint32_t*)L_FOG_END = 0x4a095440; *(uint32_t*)L_FOG_K = 0x34f88d25; *(float*)L_FOG_SCALE = 255.0f * bitsf(0x34f88d25); }
    else { *(uint32_t*)L_FOG_START = 0x461c4000; *(uint32_t*)L_FOG_END = 0x48742400; *(uint32_t*)L_FOG_K = 0x368bcf65; *(float*)L_FOG_SCALE = 255.0f * bitsf(0x368bcf65); }
    if (chance(10)) { *(float*)L_FOG_START = range(0, 1e6f); *(float*)L_FOG_END = range(0, 2e6f); }
    *(float*)L_LIGHT_R2 = chance(80) ? 562500.0f : range(0, 1e6f);
    *(float*)L_LIGHT_D = chance(80) ? 62500.0f : range(1, 1e5f);
    for (int i = 0; i < 8; i++) {
        *(int32_t*)(L_STATE + 4 * i) = (int32_t)(rnd() % 5) - 1;
        *(uint8_t*)(L_ACTIVE + i) = (uint8_t)(chance(60) ? 1 : 0);
        rnd_p3((void*)(L_POS + 12 * i), -600, 600);
        rnd_unit((void*)(L_DIR + 12 * i));
        rnd_p3((void*)(L_MPOS + 12 * i), -200, 200);
        rnd_unit((void*)(L_MDIR + 12 * i));
    }
    *(int32_t*)L_ENV = chance(40) ? 2 : chance(50) ? 1 : chance(80) ? 0 : 3;
    random_rot((float*)L_MMAT);
    *(int32_t*)L_COUNT = (int32_t)rnd();
    static const uint32_t lit_fns[] = {F_LIGHT_OFF, F_PRE_NOFOG_NOENV, F_PRE_FOG_NOENV, F_NOPRE_NOFOG_NOENV, F_NOPRE_NOFOG_SOMEENV,
                                       F_NOPRE_NOFOG_ENV, F_NOPRE_FOG_NOENV, F_NOPRE_FOG_SOMEENV, F_NOPRE_FOG_ENV};
    *(uint32_t*)L_FN_LIT = lit_fns[rnd() % 9];
    *(uint32_t*)L_FN_PRELIT = lit_fns[rnd() % 3];
    if (chance(5)) for (int i = 0; i < 0x200; i++) ((uint32_t*)L_DIFF)[i] = rnd();
    // model.obj
    *(float*)S_CLIP_NEAR = chance(80) ? range(0, 20) : range(-5, 50);
    *(float*)S_CLIP_FAR = chance(80) ? range(50, 2000) : range(0, 100);
    *(uint8_t*)S_HAS_ALPHA = (uint8_t)chance(30);
    *(uint8_t*)S_ENVMAP = (uint8_t)chance(40);
    *(uint8_t*)S_ENVPASS = (uint8_t)chance(30);
    *(uint8_t*)S_DEFERRED = (uint8_t)chance(25);
    *(int32_t*)S_ENV_TEX = (int32_t)(rnd() % 50);
    *(uint32_t*)S_MINF_VERSION = rnd() % 3;
    // the output buffer: garbage, so what a function leaves alone shows
    for (uint32_t i = 0; i < 0xbb80; i += 4) *(uint32_t*)AR(A_OUT + i) = rnd();
}
static void wildify() {
    static const uint32_t spots[] = {S_NEAR, S_FAR, S_FOV, S_CAMPOS, S_CAMPOS + 4, S_CAMDIR, L_MCAM, L_MCAM + 8, L_MSUN, L_HALF + 4,
                                     L_FOG_START, L_FOG_END, L_FOG_SCALE, L_LIGHT_D, L_MPOS + 4, L_MDIR, L_MMAT + 8, S_CLIP_NEAR,
                                     S_CLIP_FAR, L_CAMPOS, L_POS + 8, L_SUN, S_PROJ, S_CAMMAT + 4};
    const int n = 1 + (int)(rnd() % 3);
    for (int k = 0; k < n; k++) wild_at((void*)(uintptr_t)spots[rnd() % (sizeof spots / 4)]);
    for (int k = 0; k < g_ninfo; k++) {                              // a vertex or two
        uint8_t* info = info_at(k);
        const int32_t nv = *(int32_t*)info;
        float* vs = *(float**)(info + 4);
        if (nv > 0 && vs && chance(50)) wild_at(&vs[8 * (rnd() % nv) + rnd() % 8]);
    }
    if (chance(50)) wild_at(ARG(G_FRAME) + 4 * (rnd() % 12));
    if (chance(30)) wild_at(ARG(G_P3A) + 4 * (rnd() % 3));
}

// ---- the functions under test --------------------------------------------------------------------------------------
#define KINDS(X) \
    X(mrBegin, 2) X(mrEnd, 2) X(mrGetStats, 2) X(mr_ok, 2) X(mr_set_mode, 4) X(mr_restore_mode, 4) X(mr_release, 3) \
    X(mr_restore, 3) X(mrBeginFrame, 2) X(mrEndFrame, 2) X(mrBeginAlphaRects, 3) X(mrEndAlphaRects, 3) \
    X(mrDrawAlphaRect, 10) X(set_viewport, 10) X(fill_z_rect, 10) X(mrSetView5, 10) X(mrSetView4, 5) \
    X(mrSetViewBackground, 20) X(mrSetProjection, 10) X(mrMirrorView, 10) X(mrSetCamera, 20) X(mrGetCameraPos, 1) \
    X(mrCameraLook, 30) X(mrCameraDistance, 20) X(mrCameraDistanceSqr, 20) X(mrGetCamera, 3) X(mrProjectPoint, 10) \
    X(mrProjectModelPoint, 10) X(mrPointCanSee, 20) X(mrSphereCanSee, 20) X(mrBeginTriangles, 5) \
    X(mrBeginTrianglesModelSpace, 3) X(mrDrawTriangle, 5) X(mrDrawTriangles, 5) X(mrDrawIndexTriangles, 5) \
    X(mrEndTriangles, 2) X(mr_connect_3d, 5) X(mr_disconnect_3d, 3) X(mr_release_3d, 3) X(mr_restore_3d, 3) \
    X(init_render_state, 3) X(clear_render_state, 1) X(init_camera, 2) X(calc_projection_matrix, 30) \
    X(dxMatrixBegin, 2) X(dxMatrixSetModel, 3) X(dxMatrixSetCamera, 3) X(dxMatrixSetProjection, 3) \
    X(mr_feature_begin, 2) X(mrEnable, 40) X(mrDisable, 40) X(mrIsEnabled, 20) X(mrSetBlendMode, 20) X(mrPushState, 10) \
    X(mrPopState, 20) \
    X(mr_light_begin, 3) X(mr_light_end, 1) X(mr_light_release, 1) X(mr_light_restore, 3) X(light_begin, 10) \
    X(mrLightPlaceCamera, 10) X(mrLightPlaceModel, 40) X(mrLightVerts, 40) X(calc_day, 80) X(calc_night, 80) \
    X(nocalc_light, 10) X(mrLightFogVerts, 40) X(calc_night_2, 30) X(mrLightSetAlpha, 20) X(mrLightEnv, 5) \
    X(mrLightCalc, 5) X(mrLightCalcFog, 5) X(mrLightCalcSpecular, 5) X(mrLightMode, 10) X(mrLightSpecial, 10) \
    X(make_diffuse_table, 10) X(make_specular_table, 10) X(set_light_state, 10) X(LightSurfaceLit, 20) \
    X(LightSurfacePreLit, 20) X(light_off, 10) X(light_pre_nofog_noenv, 10) X(light_pre_nofog_someenv, 2) \
    X(light_pre_nofog_env, 2) X(light_pre_fog_noenv, 20) X(light_pre_fog_someenv, 2) X(light_pre_fog_env, 2) \
    X(light_nopre_nofog_noenv, 20) X(light_nopre_nofog_someenv, 30) X(light_nopre_nofog_env, 50) \
    X(light_nopre_fog_noenv, 30) X(light_nopre_fog_someenv, 40) X(light_nopre_fog_env, 40) X(DotProduct, 5) \
    X(VectorAddScaled, 5) X(APPLY_ENVMAP, 30) X(APPLY_FOG, 30) \
    X(mr_model_begin, 3) X(mr_model_end, 3) X(mr_model_first, 2) X(mr_model_last, 2) X(mrModelLoad, 10) \
    X(mrModelLoadRemap, 10) X(mrModelUnload, 5) X(mrModelRemapTextures, 10) X(mrModelCopyTransformFrame, 10) \
    X(mrModelCopyRemap, 10) X(mrModelCopyTransformMatrix, 10) X(mrModelCopy, 5) X(mrModelDestroyCopy, 5) \
    X(mrModelCreate, 10) X(mrModelDestroy, 10) X(mrModelBuild, 20) X(mrModelBuildLit, 10) \
    X(mrModelEnterDeferredMode, 2) X(mrModelLeaveDeferredMode, 20) X(mrModelEnvMap, 2) X(mrModelDrawFrame, 60) \
    X(mrModelDraw1, 20) X(direct_model_build, 30) X(set_texture_id, 30) X(direct_model_draw, 40) X(mrModelSetFrame, 10) \
    X(mrModelInfoGet, 20) X(mrModelInfoForget, 2) X(mrModelInfoSave, 10) X(mrModelPick, 30) X(mrModelPickVertex, 20) \
    X(mrModelPickEdge, 20) X(line_point_dist, 5) X(mr_model_set_frame, 5) X(mrModelGetExtents, 20) X(mrModelGetInfo, 2) \
    X(mrModelGetVerts, 3) X(mrModelHasAlpha, 1) X(mrModelSetAlpha, 5) X(mrModelClearAlpha, 3) X(mrModelSetClipDist, 2) \
    X(mrModelFlush, 10) X(convert_to_mrmodel, 1) X(convert_to_mip, 1) X(begin_deferred, 5) X(end_deferred, 5) \
    X(begin_deferred_surfs, 2) X(end_deferred_surfs, 30) X(draw_alpha_deferred_surfs, 20) X(add_deferred_surf, 30) \
    X(model_copy_transform, 20) X(Pool_surf_sdtor, 3) X(Pool_model_sdtor, 3) X(Pool_deferred_sdtor, 3) \
    X(SEQ_DEFERRED, 40) X(SEQ_LIFTED, 40) X(SEQ_FEATURES, 20) X(SEQ_FRAME, 20)
#define KIND_ENUM(n, w) K_##n,
enum Kind { KINDS(KIND_ENUM) N_KINDS };
#define KIND_NAME(n, w) #n,
static const char* const kind_names[N_KINDS] = {KINDS(KIND_NAME)};
#define KIND_W(n, w) w,
static const int kind_weights[N_KINDS] = {KINDS(KIND_W)};

struct Args {
    uint8_t* fr; float* pa; float* pb; float* pc; float* out; float* mat;
    int i[6]; float f[3]; uint8_t b;
    int m;                          // a model
    uint8_t* info;                  // an mrModelInfo
    uint8_t* surf;                  // one of its surfaces
    uint8_t* si;                    // a model_surface_info of m
    uint8_t* lout;                  // an output vertex buffer
    const char* name;
    uint8_t* remap; int nremap;
    int feature;
    int* outs;
    int nseq;
    int r[8];                       // choices made up front (never draw random numbers during a pass)
    float fov;
};
static Args g_args;
static uint8_t* model_of(int m) { return (uint8_t*)(uintptr_t)m; }
static void random_args(Kind k) {
    Args& g = g_args;
    memset(&g, 0, sizeof g);
    g.fr = ARG(G_FRAME);
    random_frame((float*)g.fr, chance(50) ? 50.0f : 800.0f);
    g.pa = (float*)ARG(G_P3A); g.pb = (float*)ARG(G_P3B); g.pc = (float*)ARG(G_P3C);
    rnd_p3(g.pa, -200, 200); rnd_p3(g.pb, -200, 200); rnd_p3(g.pc, -200, 200);
    if (chance(5)) memcpy(g.pb, g.pa, 12);
    g.out = (float*)ARG(G_OUT);
    for (int q = 0; q < 16; q++) ((uint32_t*)g.out)[q] = rnd();
    g.mat = (float*)ARG(G_MAT);
    random_rot(g.mat);
    for (int q = 0; q < 6; q++) g.i[q] = chance(80) ? (int)(rnd() % 800) - 50 : (int)rnd();
    for (int q = 0; q < 3; q++) g.f[q] = chance(80) ? range(0, 1) : chance(50) ? range(-3, 3) : wildf();
    g.b = (uint8_t)(chance(80) ? rnd() % 2 : rnd());
    g.m = g_nmodels ? g_models[rnd() % g_nmodels] : 0;
    g.info = info_at((int)(rnd() % g_ninfo));
    if (chance(40) && g.m && *(uint8_t**)(model_of(g.m) + 0x10)) g.info = *(uint8_t**)(model_of(g.m) + 0x10);
    const int32_t ns = *(int32_t*)(g.info + 8);
    uint8_t* surfs = *(uint8_t**)(g.info + 0xc);
    g.surf = ns > 0 && surfs ? surfs + 32 * (rnd() % ns) : ARG(G_SURF);
    if (g.surf == ARG(G_SURF)) {                                     // a model with no surfaces: a sane one of its own
        memset(g.surf, 0, 32);
        const int32_t nv = *(int32_t*)g.info;
        g.surf[0x10] = (uint8_t)(rnd() % 4);
        *(int16_t*)(g.surf + 0x1a) = (int16_t)(nv < 20 ? nv : 20);
    }
    if (g.m) {
        uint8_t* s = *(uint8_t**)(model_of(g.m) + 0x28);
        int n = s ? (int)(rnd() % 4) : 0;
        while (s && n-- > 0 && *(uint8_t**)(s + 0x28)) s = *(uint8_t**)(s + 0x28);
        g.si = s;
    }
    g.lout = chance(50) ? AR(A_OUT) : *(uint8_t**)S_LIT_BUF;
    if (!g.lout) g.lout = AR(A_OUT);
    strcpy((char*)ARG(G_NAME), k_names[rnd() % 8]);
    g.name = (const char*)ARG(G_NAME);
    g.remap = ARG(G_REMAP);
    g.nremap = (int)(rnd() % 5);
    for (int e = 0; e < 5; e++) {
        strncpy((char*)g.remap + 0x28 * e, k_names[rnd() % 8], 16);
        strncpy((char*)g.remap + 0x28 * e + 0x10, k_names[rnd() % 8], 16);
        *(int32_t*)(g.remap + 0x28 * e + 0x20) = (int32_t)(rnd() % 3);
        *(uint8_t**)(g.remap + 0x28 * e + 0x24) = chance(70) ? ARG(G_REMAP_OUT) + 4 * e : 0;
    }
    static const int features[] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800};
    g.feature = chance(95) ? features[rnd() % 12] : (int)(rnd() % 0x2000);
    g.outs = (int*)ARG(G_INTS);
    for (int q = 0; q < 8; q++) g.outs[q] = (int)rnd();
    // mrDrawTriangles' vertices and indices
    for (int q = 0; q < 0x400; q += 4) *(uint32_t*)ARG(G_TRIS + q) = rnd();
    for (int q = 0; q < 0x100; q += 2) *(uint16_t*)ARG(G_IDX + q) = (uint16_t)(rnd() % 32);
    g.nseq = 1 + (int)(rnd() % 12);
    for (int q = 0; q < 8; q++) g.r[q] = (int)rnd();
    g.fov = chance(1) ? wildf() : chance(90) ? g.f[2] * 2.5f : range(-40, 40);
    switch (k) {
    case K_mrModelInfoGet: case K_mrModelLoad: case K_mrModelLoadRemap:
        g_script.res_found = (uint8_t)chance(90);
        g_script.res_fresh = (uint8_t)chance(70);
        g_script.res_version = chance(45) ? 0 : chance(90) ? 1 : 2 + rnd() % 3;
        break;
    case K_mrModelPick: case K_mrModelPickVertex: case K_mrModelPickEdge:
        g.info = info_at((int)(rnd() % g_ninfo));
        g.i[0] = 300 + (int)(rnd() % 60); g.i[1] = 200 + (int)(rnd() % 60);
        break;
    case K_add_deferred_surf:
        if (g.si && chance(70)) *(int32_t*)(g.si + 0x14) = chance(80) ? (int32_t)(rnd() % 118) - 1 : (int32_t)(rnd() % 140);
        break;
    default: break;
    }
    g_script.dx_ok = (uint8_t)chance(80);
    g_script.tex_ok = (uint8_t)chance(80);
    g_script.file_ok = (uint8_t)chance(80);
    g_script.deres_frames = (int)(rnd() % 3);
    g_script.proj_fail_pct = chance(50) ? 5 : 40;
}

#define ORIG(fn) ((decltype(&fn))VP_CAT(addr_, fn))
#define CALLV(fn, ...) do { if (rw) fn(__VA_ARGS__); else ORIG(fn)(__VA_ARGS__); } while (0)
#define CALLR(fn, ...) (rw ? rv(fn(__VA_ARGS__)) : rv(ORIG(fn)(__VA_ARGS__)))
static uint64_t rv(uint8_t v) { return v; }
static uint64_t rv(int v) { return (uint32_t)v; }
static uint64_t rv(void* v) { return rel(v); }
static uint64_t rv(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static void seq_lifted_patch(bool on) {                              // begin_deferred's operands, as M1 patches them
    *(uint32_t*)0x0045723e = on ? (uint32_t)(uintptr_t)AR(A_LIFT) : 0x00522b40;
    *(uint32_t*)0x00457245 = on ? 1024u : 0x78u;
}
static int g_seq_ids[64];
static uint8_t g_alpha_prologue[5];                                  // draw_alpha_deferred_surfs' first bytes
static uint64_t call_kind(Kind k, bool rw) {
    Args& g = g_args;
    uint8_t* info = g.info;
    uint8_t* in = *(uint8_t**)(info + 4);
    switch (k) {
    case K_mrBegin: CALLV(mrBegin); return 0;
    case K_mrEnd: CALLV(mrEnd); return 0;
    case K_mrGetStats: CALLV(mrGetStats, g.outs, g.outs + 1, g.outs + 2, g.outs + 3); return 0;
    case K_mr_ok: return CALLR(mr_ok);
    case K_mr_set_mode: CALLV(mr_set_mode); return 0;
    case K_mr_restore_mode: CALLV(mr_restore_mode); return 0;
    case K_mr_release: CALLV(mr_release); return 0;
    case K_mr_restore: CALLV(mr_restore); return 0;
    case K_mrBeginFrame: CALLV(mrBeginFrame); return 0;
    case K_mrEndFrame: CALLV(mrEndFrame); return 0;
    case K_mrBeginAlphaRects: CALLV(mrBeginAlphaRects); return 0;
    case K_mrEndAlphaRects: CALLV(mrEndAlphaRects); return 0;
    case K_mrDrawAlphaRect: CALLV(mrDrawAlphaRect, g.i[0], g.i[1], g.i[2], g.i[3], (uint32_t)g.i[4]); return 0;
    case K_set_viewport: CALLV(set_viewport, g.i[0], g.i[1], g.i[2], g.i[3]); return 0;
    case K_fill_z_rect: CALLV(fill_z_rect, g.i[0], g.i[1], g.i[2], g.i[3]); return 0;
    case K_mrSetView5: CALLV(mrSetView5, g.i[0], g.i[1], g.i[2], g.i[3], g.b); return 0;
    case K_mrSetView4: CALLV(mrSetView4, g.i[0], g.i[1], g.i[2], g.i[3]); return 0;
    case K_mrSetViewBackground: CALLV(mrSetViewBackground, g.f[0], g.f[1], g.f[2]); return 0;
    case K_mrSetProjection: CALLV(mrSetProjection, fbits_h(g.f[0]), fbits_h(g.f[1] * 1000.0f), fbits_h(g.f[2] * 2.0f)); return 0;
    case K_mrMirrorView: CALLV(mrMirrorView, g.b); return 0;
    case K_mrSetCamera: CALLV(mrSetCamera, g.fr); return 0;
    case K_mrGetCameraPos: return CALLR(mrGetCameraPos);
    case K_mrCameraLook: CALLV(mrCameraLook, g.pa, g.pb); return 0;
    case K_mrCameraDistance: return CALLR(mrCameraDistance, g.pa);
    case K_mrCameraDistanceSqr: return CALLR(mrCameraDistanceSqr, g.pa);
    case K_mrGetCamera: CALLV(mrGetCamera, g.out); return 0;
    case K_mrProjectPoint: return CALLR(mrProjectPoint, g.pa, g.out);
    case K_mrProjectModelPoint: return CALLR(mrProjectModelPoint, g.pa, g.out, g.fr, g.b);
    case K_mrPointCanSee: return CALLR(mrPointCanSee, g.pa);
    case K_mrSphereCanSee: return CALLR(mrSphereCanSee, g.pa, g.f[0] * 100.0f);
    case K_mrBeginTriangles: CALLV(mrBeginTriangles, g.i[0] % 64); return 0;
    case K_mrBeginTrianglesModelSpace: CALLV(mrBeginTrianglesModelSpace, g.i[0] % 64); return 0;
    case K_mrDrawTriangle: CALLV(mrDrawTriangle, ARG(G_TRIS)); return 0;
    case K_mrDrawTriangles: CALLV(mrDrawTriangles, ARG(G_TRIS), (g.i[0] & 31)); return 0;
    case K_mrDrawIndexTriangles: CALLV(mrDrawIndexTriangles, ARG(G_TRIS), (g.i[0] & 31), (const uint16_t*)ARG(G_IDX), g.i[1] & 63); return 0;
    case K_mrEndTriangles: CALLV(mrEndTriangles); return 0;
    case K_mr_connect_3d: CALLV(mr_connect_3d); return 0;
    case K_mr_disconnect_3d: CALLV(mr_disconnect_3d); return 0;
    case K_mr_release_3d: CALLV(mr_release_3d); return 0;
    case K_mr_restore_3d: CALLV(mr_restore_3d); return 0;
    case K_init_render_state: CALLV(init_render_state); return 0;
    case K_clear_render_state: CALLV(clear_render_state); return 0;
    case K_init_camera: CALLV(init_camera); return 0;
    case K_calc_projection_matrix: CALLV(calc_projection_matrix, g.f[0], 1.0f + g.f[1] * 1000.0f, g.fov); return 0;
    case K_dxMatrixBegin: CALLV(dxMatrixBegin); return 0;
    case K_dxMatrixSetModel: CALLV(dxMatrixSetModel, g.mat); return 0;
    case K_dxMatrixSetCamera: CALLV(dxMatrixSetCamera, g.mat); return 0;
    case K_dxMatrixSetProjection: CALLV(dxMatrixSetProjection, g.mat); return 0;
    case K_mr_feature_begin: CALLV(mr_feature_begin); return 0;
    case K_mrEnable: CALLV(mrEnable, g.feature); return 0;
    case K_mrDisable: CALLV(mrDisable, g.feature); return 0;
    case K_mrIsEnabled: return CALLR(mrIsEnabled, g.feature);
    case K_mrSetBlendMode: CALLV(mrSetBlendMode, (uint32_t)g.r[0] % 10 < 9 ? (int)((uint32_t)g.r[1] % 5) : g.i[0]); return 0;
    case K_mrPushState: CALLV(mrPushState); return 0;
    case K_mrPopState: CALLV(mrPopState); return 0;
    case K_mr_light_begin: CALLV(mr_light_begin); return 0;
    case K_mr_light_end: CALLV(mr_light_end); return 0;
    case K_mr_light_release: CALLV(mr_light_release); return 0;
    case K_mr_light_restore: CALLV(mr_light_restore); return 0;
    case K_light_begin: CALLV(light_begin); return 0;
    case K_mrLightPlaceCamera: CALLV(mrLightPlaceCamera, g.pa, g.pb); return 0;
    case K_mrLightPlaceModel: CALLV(mrLightPlaceModel, g.fr); return 0;
    case K_mrLightVerts: CALLV(mrLightVerts, info, g.lout); return 0;
    case K_calc_day: CALLV(calc_day, info, g.lout); return 0;
    case K_calc_night: CALLV(calc_night, info, g.lout); return 0;
    case K_nocalc_light: CALLV(nocalc_light, info, g.lout); return 0;
    case K_mrLightFogVerts: CALLV(mrLightFogVerts, info, g.lout); return 0;
    case K_calc_night_2: CALLV(calc_night_2, info, g.lout); return 0;
    case K_mrLightSetAlpha: CALLV(mrLightSetAlpha, g.f[0]); return 0;
    case K_mrLightEnv: CALLV(mrLightEnv, (int)((uint32_t)g.r[0] % 4)); return 0;
    case K_mrLightCalc: CALLV(mrLightCalc, g.b); return 0;
    case K_mrLightCalcFog: CALLV(mrLightCalcFog, g.b); return 0;
    case K_mrLightCalcSpecular: CALLV(mrLightCalcSpecular, g.b); return 0;
    case K_mrLightMode: CALLV(mrLightMode, (int)((uint32_t)g.r[0] % 3)); return 0;
    case K_mrLightSpecial: CALLV(mrLightSpecial, (int)((uint32_t)g.r[0] % 8), g.fr); return 0;
    case K_make_diffuse_table: CALLV(make_diffuse_table, (uint32_t)g.r[0] % 10 < 7 ? 1.0f : g.f[0] * 2.0f); return 0;
    case K_make_specular_table: CALLV(make_specular_table, (uint32_t)g.r[0] % 10 < 7 ? 1.0f : g.f[0] * 1.5f); return 0;
    case K_set_light_state: CALLV(set_light_state); return 0;
    case K_LightSurfaceLit: CALLV(LightSurfaceLit, g.surf, in, g.lout); return 0;
    case K_LightSurfacePreLit: CALLV(LightSurfacePreLit, g.surf, in, g.lout); return 0;
    case K_light_off: CALLV(light_off, g.surf, in, g.lout); return 0;
    case K_light_pre_nofog_noenv: CALLV(light_pre_nofog_noenv, g.surf, in, g.lout); return 0;
    case K_light_pre_nofog_someenv: CALLV(light_pre_nofog_someenv, g.surf, in, g.lout); return 0;
    case K_light_pre_nofog_env: CALLV(light_pre_nofog_env, g.surf, in, g.lout); return 0;
    case K_light_pre_fog_noenv: CALLV(light_pre_fog_noenv, g.surf, in, g.lout); return 0;
    case K_light_pre_fog_someenv: CALLV(light_pre_fog_someenv, g.surf, in, g.lout); return 0;
    case K_light_pre_fog_env: CALLV(light_pre_fog_env, g.surf, in, g.lout); return 0;
    case K_light_nopre_nofog_noenv: CALLV(light_nopre_nofog_noenv, g.surf, in, g.lout); return 0;
    case K_light_nopre_nofog_someenv: CALLV(light_nopre_nofog_someenv, g.surf, in, g.lout); return 0;
    case K_light_nopre_nofog_env: CALLV(light_nopre_nofog_env, g.surf, in, g.lout); return 0;
    case K_light_nopre_fog_noenv: CALLV(light_nopre_fog_noenv, g.surf, in, g.lout); return 0;
    case K_light_nopre_fog_someenv: CALLV(light_nopre_fog_someenv, g.surf, in, g.lout); return 0;
    case K_light_nopre_fog_env: CALLV(light_nopre_fog_env, g.surf, in, g.lout); return 0;
    case K_DotProduct: return CALLR(DotProduct, (const P3*)g.pa, (const P3*)g.pb);
    case K_VectorAddScaled: CALLV(VectorAddScaled, (P3*)((uint32_t)g.r[0] % 5 == 0 ? g.pa : g.out), (const P3*)g.pa, (const P3*)g.pb, g.f[0]); return 0;
    case K_APPLY_ENVMAP: if (!in) return 0; CALLV(APPLY_ENVMAP, in, g.lout); return 0;
    case K_APPLY_FOG: if (!in) return 0; CALLV(APPLY_FOG, in, g.lout); return 0;
    case K_mr_model_begin: CALLV(mr_model_begin); return 0;
    case K_mr_model_end: CALLV(mr_model_end); return 0;
    case K_mr_model_first: CALLV(mr_model_first); return 0;
    case K_mr_model_last: CALLV(mr_model_last); return 0;
    case K_mrModelLoad: return CALLR(mrModelLoad, g.name);
    case K_mrModelLoadRemap: return CALLR(mrModelLoadRemap, g.name, g.remap, g.nremap);
    case K_mrModelUnload: if (!g.m) return 0; CALLV(mrModelUnload, g.m); return 0;
    case K_mrModelRemapTextures: if (!g.m) return 0; CALLV(mrModelRemapTextures, g.m, g.remap, g.nremap); return 0;
    case K_mrModelCopyTransformFrame: if (!g.m) return 0; return CALLR(mrModelCopyTransformFrame, g.m, g.fr);
    case K_mrModelCopyRemap: if (!g.m) return 0; return CALLR(mrModelCopyRemap, g.m, g.remap, g.nremap);
    case K_mrModelCopyTransformMatrix: if (!g.m) return 0; return CALLR(mrModelCopyTransformMatrix, g.m, g.mat);
    case K_mrModelCopy: if (!g.m) return 0; return CALLR(mrModelCopy, g.m);
    case K_mrModelDestroyCopy: if (!g.m) return 0; CALLV(mrModelDestroyCopy, g.m); return 0;
    case K_mrModelCreate: return CALLR(mrModelCreate, g.name, (int)((uint32_t)g.r[0] % 50));
    case K_mrModelDestroy: if (!g.m) return 0; CALLV(mrModelDestroy, g.m); return 0;
    case K_mrModelBuild: if (!g.m) return 0; CALLV(mrModelBuild, g.m, info, (int)((uint32_t)g.r[0] % 8)); return 0;
    case K_mrModelBuildLit: if (!g.m) return 0; CALLV(mrModelBuildLit, g.m, info, (int)((uint32_t)g.r[0] % 8)); return 0;
    case K_mrModelEnterDeferredMode: CALLV(mrModelEnterDeferredMode); return 0;
    case K_mrModelLeaveDeferredMode: CALLV(mrModelLeaveDeferredMode); return 0;
    case K_mrModelEnvMap: CALLV(mrModelEnvMap, g.b); return 0;
    case K_mrModelDrawFrame: if (!g.m) return 0; CALLV(mrModelDrawFrame, g.m, g.fr); return 0;
    case K_mrModelDraw1: if (!g.m) return 0; CALLV(mrModelDraw1, g.m); return 0;
    case K_direct_model_build: if (!g.m) return 0; CALLV(direct_model_build, g.m, info, g.b, (int)((uint32_t)g.r[0] % 8)); return 0;
    case K_set_texture_id: if (!g.m || !g.si) return 0; CALLV(set_texture_id, g.m, g.r[0] & 1 ? (const char*)g.si : g.name, g.si); return 0;
    case K_direct_model_draw: if (!g.m) return 0; CALLV(direct_model_draw, g.m); return 0;
    case K_mrModelSetFrame: CALLV(mrModelSetFrame, g.fr); return 0;
    case K_mrModelInfoGet: return CALLR(mrModelInfoGet, g.name);
    case K_mrModelInfoForget: CALLV(mrModelInfoForget, info); return 0;
    case K_mrModelInfoSave: return CALLR(mrModelInfoSave, info, g.name);
    case K_mrModelPick: return CALLR(mrModelPick, info, g.fr, g.i[0], g.i[1]);
    case K_mrModelPickVertex: return CALLR(mrModelPickVertex, info, g.fr, g.i[0], g.i[1], g.outs, g.outs + 1);
    case K_mrModelPickEdge: return CALLR(mrModelPickEdge, info, g.fr, g.i[0], g.i[1], g.outs, g.outs + 1);
    case K_line_point_dist: return CALLR(line_point_dist, g.i[0], g.i[1], (const P3*)g.pa, (const P3*)g.pb);
    case K_mr_model_set_frame: CALLV(mr_model_set_frame, g.fr); return 0;
    case K_mrModelGetExtents: if (!g.m) return 0;
        CALLV(mrModelGetExtents, g.m, (float*)g.outs, (float*)g.outs + 1, (float*)g.outs + 2, (float*)g.outs + 3, (float*)g.outs + 4, (float*)g.outs + 5);
        return 0;
    case K_mrModelGetInfo: if (!g.m) return 0; return CALLR(mrModelGetInfo, g.m);
    case K_mrModelGetVerts: if (!g.m) return 0; CALLV(mrModelGetVerts, g.m, (uint8_t**)g.outs, g.outs + 1); return 0;
    case K_mrModelHasAlpha: return CALLR(mrModelHasAlpha);
    case K_mrModelSetAlpha: CALLV(mrModelSetAlpha, g.f[0]); return 0;
    case K_mrModelClearAlpha: CALLV(mrModelClearAlpha); return 0;
    case K_mrModelSetClipDist: CALLV(mrModelSetClipDist, g.f[0], g.f[1]); return 0;
    case K_mrModelFlush: CALLV(mrModelFlush); return 0;
    case K_convert_to_mrmodel: return CALLR(convert_to_mrmodel, (uint32_t)g.m);
    case K_convert_to_mip: return CALLR(convert_to_mip, g.m);
    case K_begin_deferred: CALLV(begin_deferred); return 0;
    case K_end_deferred: CALLV(end_deferred); return 0;
    case K_begin_deferred_surfs: CALLV(begin_deferred_surfs); return 0;
    case K_end_deferred_surfs: CALLV(end_deferred_surfs); return 0;
    case K_draw_alpha_deferred_surfs: CALLV(draw_alpha_deferred_surfs); return 0;
    case K_add_deferred_surf: if (!g.si || !g.m) return 0; CALLV(add_deferred_surf, *(uint8_t**)(model_of(g.m) + 0x10), g.surf, g.si); return 0;
    case K_model_copy_transform: if (!g.m) return 0; return CALLR(model_copy_transform, g.m, g.fr, (uint8_t**)g.outs);
    case K_Pool_surf_sdtor: { void* p = *(void**)S_SURF_POOL; if (!p) return 0; return rel(rw ? Pool_surf_sdtor(p, 0, g.b) : ORIG(Pool_surf_sdtor)(p, 0, g.b)); }
    case K_Pool_model_sdtor: { void* p = *(void**)S_MODEL_POOL; if (!p) return 0; return rel(rw ? Pool_model_sdtor(p, 0, g.b) : ORIG(Pool_model_sdtor)(p, 0, g.b)); }
    case K_Pool_deferred_sdtor: { void* p = *(void**)S_DEFER_POOL; if (!p) return 0; return rel(rw ? Pool_deferred_sdtor(p, 0, g.b) : ORIG(Pool_deferred_sdtor)(p, 0, g.b)); }
    case K_SEQ_DEFERRED: case K_SEQ_LIFTED: {
        // surfaces of the models queued (texture numbers set per step), then drawn: the stock buckets -- numbers past
        // the array included -- or the lifted ones against the stock (numbers below 118)
        const bool lifted = k == K_SEQ_LIFTED;
        // lifted: end_deferred_surfs calls draw_alpha_deferred_surfs by address, which in the game is its rewrite (all
        // three are hooked together); here the rewrite's pass hooks it for the sequence
        if (lifted && rw) patch_jmp(0x00457340, (void*)&draw_alpha_deferred_surfs);   // (main puts it back)
        CALLV(begin_deferred_surfs);
        for (int q = 0; q < g.nseq && g_nmodels; q++) {
            uint8_t* mi = model_of(g_models[q % g_nmodels]);
            uint8_t* inf = *(uint8_t**)(mi + 0x10);
            uint8_t* si = *(uint8_t**)(mi + 0x28);
            if (!si || !inf || *(int32_t*)(inf + 8) <= 0) continue;
            *(int32_t*)(si + 0x14) = g_seq_ids[q];
            CALLV(add_deferred_surf, inf, *(uint8_t**)(inf + 0xc), si);
        }
        if (lifted || (uint32_t)g.r[0] % 5) CALLV(end_deferred_surfs);
        else CALLV(draw_alpha_deferred_surfs);
        return 0;
    }
    case K_SEQ_FEATURES:
        for (int q = 0; q < g.nseq; q++) {
            static const int features[] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800};
            const int f = features[((uint32_t)g.i[q % 6] + q) % 12];
            switch (((uint32_t)g.i[(q + 1) % 6] + q) % 5) {
            case 0: CALLV(mrEnable, f); break;
            case 1: CALLV(mrDisable, f); break;
            case 2: if (*(int32_t*)S_FSTACK_IDX < 4) CALLV(mrPushState); break;
            case 3: if (*(int32_t*)S_FSTACK_IDX > 0) CALLV(mrPopState); break;
            default: CALLV(mrSetBlendMode, 1 + q % 2); break;
            }
        }
        return 0;
    case K_SEQ_FRAME:                                                // a frame's worth: camera, models, lighting, draw
        CALLV(mrCameraLook, g.pa, g.pb);
        CALLV(mrLightMode, g.i[0] & 1);
        for (int q = 0; q < g_nmodels; q++) {
            CALLV(mrModelSetFrame, g.fr);
            CALLV(mrModelDrawFrame, g_models[q], g.fr);
        }
        return 0;
    default: return 0;
    }
}
static bool returns_byte(Kind k) {
    return k == K_mr_ok || k == K_mrProjectPoint || k == K_mrProjectModelPoint || k == K_mrPointCanSee || k == K_mrSphereCanSee ||
           k == K_mrIsEnabled || k == K_mrModelInfoSave || k == K_mrModelPickVertex || k == K_mrModelPickEdge || k == K_mrModelHasAlpha;
}

#define FP(fn, ...) VP_CAT(fpof_, fn)(fp, __VA_ARGS__)
#define FP0(fn) VP_CAT(fpof_, fn)(fp)
static void footprint_of(Kind k, Footprint& fp) {
    fp.n = 0;
    fp.replay_only = 0;
    fp.pure = false;
    Args& g = g_args;
    uint8_t* info = g.info;
    uint8_t* in = *(uint8_t**)(info + 4);
    switch (k) {
    case K_mrBegin: FP0(mrBegin); break;
    case K_mrEnd: FP0(mrEnd); break;
    case K_mrGetStats: FP(mrGetStats, g.outs, g.outs + 1, g.outs + 2, g.outs + 3); break;
    case K_mr_ok: FP0(mr_ok); break;
    case K_mr_set_mode: FP0(mr_set_mode); break;
    case K_mr_restore_mode: FP0(mr_restore_mode); break;
    case K_mr_release: FP0(mr_release); break;
    case K_mr_restore: FP0(mr_restore); break;
    case K_mrBeginFrame: FP0(mrBeginFrame); break;
    case K_mrEndFrame: FP0(mrEndFrame); break;
    case K_mrBeginAlphaRects: FP0(mrBeginAlphaRects); break;
    case K_mrEndAlphaRects: FP0(mrEndAlphaRects); break;
    case K_mrDrawAlphaRect: FP(mrDrawAlphaRect, 0, 0, 0, 0, 0); break;
    case K_set_viewport: FP(set_viewport, 0, 0, 0, 0); break;
    case K_fill_z_rect: FP(fill_z_rect, 0, 0, 0, 0); break;
    case K_mrSetView5: FP(mrSetView5, 0, 0, 0, 0, 0); break;
    case K_mrSetView4: FP(mrSetView4, 0, 0, 0, 0); break;
    case K_mrSetViewBackground: FP(mrSetViewBackground, 0, 0, 0); break;
    case K_mrSetProjection: FP(mrSetProjection, 0, 0, 0); break;
    case K_mrMirrorView: FP(mrMirrorView, 0); break;
    case K_mrSetCamera: FP(mrSetCamera, g.fr); break;
    case K_mrGetCameraPos: FP0(mrGetCameraPos); break;
    case K_mrCameraLook: FP(mrCameraLook, g.pa, g.pb); break;
    case K_mrCameraDistance: FP(mrCameraDistance, g.pa); break;
    case K_mrCameraDistanceSqr: FP(mrCameraDistanceSqr, g.pa); break;
    case K_mrGetCamera: FP(mrGetCamera, g.out); break;
    case K_mrProjectPoint: FP(mrProjectPoint, g.pa, g.out); break;
    case K_mrProjectModelPoint: FP(mrProjectModelPoint, g.pa, g.out, g.fr, 0); break;
    case K_mrPointCanSee: FP(mrPointCanSee, g.pa); break;
    case K_mrSphereCanSee: FP(mrSphereCanSee, g.pa, 0); break;
    case K_mrBeginTriangles: FP(mrBeginTriangles, 0); break;
    case K_mrBeginTrianglesModelSpace: FP(mrBeginTrianglesModelSpace, 0); break;
    case K_mrDrawTriangle: FP(mrDrawTriangle, 0); break;
    case K_mrDrawTriangles: FP(mrDrawTriangles, 0, 0); break;
    case K_mrDrawIndexTriangles: FP(mrDrawIndexTriangles, 0, 0, 0, 0); break;
    case K_mrEndTriangles: FP0(mrEndTriangles); break;
    case K_mr_connect_3d: FP0(mr_connect_3d); break;
    case K_mr_disconnect_3d: FP0(mr_disconnect_3d); break;
    case K_mr_release_3d: FP0(mr_release_3d); break;
    case K_mr_restore_3d: FP0(mr_restore_3d); break;
    case K_init_render_state: FP0(init_render_state); break;
    case K_clear_render_state: FP0(clear_render_state); break;
    case K_init_camera: FP0(init_camera); break;
    case K_calc_projection_matrix: FP(calc_projection_matrix, 0, 0, 0); break;
    case K_dxMatrixBegin: FP0(dxMatrixBegin); break;
    case K_dxMatrixSetModel: FP(dxMatrixSetModel, g.mat); break;
    case K_dxMatrixSetCamera: FP(dxMatrixSetCamera, g.mat); break;
    case K_dxMatrixSetProjection: FP(dxMatrixSetProjection, g.mat); break;
    case K_mr_feature_begin: FP0(mr_feature_begin); break;
    case K_mrEnable: FP(mrEnable, g.feature); break;
    case K_mrDisable: FP(mrDisable, g.feature); break;
    case K_mrIsEnabled: FP(mrIsEnabled, g.feature); break;
    case K_mrSetBlendMode: FP(mrSetBlendMode, 0); break;
    case K_mrPushState: FP0(mrPushState); break;
    case K_mrPopState: FP0(mrPopState); break;
    case K_mr_light_begin: FP0(mr_light_begin); break;
    case K_mr_light_end: FP0(mr_light_end); break;
    case K_mr_light_release: FP0(mr_light_release); break;
    case K_mr_light_restore: FP0(mr_light_restore); break;
    case K_light_begin: FP0(light_begin); break;
    case K_mrLightPlaceCamera: FP(mrLightPlaceCamera, g.pa, g.pb); break;
    case K_mrLightPlaceModel: FP(mrLightPlaceModel, g.fr); break;
    case K_mrLightVerts: FP(mrLightVerts, info, g.lout); break;
    case K_calc_day: FP(calc_day, info, g.lout); break;
    case K_calc_night: FP(calc_night, info, g.lout); break;
    case K_nocalc_light: FP(nocalc_light, info, g.lout); break;
    case K_mrLightFogVerts: FP(mrLightFogVerts, info, g.lout); break;
    case K_calc_night_2: FP(calc_night_2, info, g.lout); break;
    case K_mrLightSetAlpha: FP(mrLightSetAlpha, 0); break;
    case K_mrLightEnv: FP(mrLightEnv, 0); break;
    case K_mrLightCalc: FP(mrLightCalc, 0); break;
    case K_mrLightCalcFog: FP(mrLightCalcFog, 0); break;
    case K_mrLightCalcSpecular: FP(mrLightCalcSpecular, 0); break;
    case K_mrLightMode: FP(mrLightMode, 0); break;
    case K_mrLightSpecial: FP(mrLightSpecial, (int)((uint32_t)g.r[0] % 8), g.fr); break;
    case K_make_diffuse_table: FP(make_diffuse_table, 0); break;
    case K_make_specular_table: FP(make_specular_table, 0); break;
    case K_set_light_state: FP0(set_light_state); break;
    case K_LightSurfaceLit: FP(LightSurfaceLit, g.surf, in, g.lout); break;
    case K_LightSurfacePreLit: FP(LightSurfacePreLit, g.surf, in, g.lout); break;
    case K_light_off: case K_light_pre_nofog_noenv: case K_light_pre_nofog_someenv: case K_light_pre_nofog_env:
    case K_light_pre_fog_noenv: case K_light_pre_fog_someenv: case K_light_pre_fog_env: case K_light_nopre_nofog_noenv:
    case K_light_nopre_nofog_someenv: case K_light_nopre_nofog_env: case K_light_nopre_fog_noenv:
    case K_light_nopre_fog_someenv: case K_light_nopre_fog_env:
        FP(light_off, g.surf, in, g.lout); break;
    case K_DotProduct: FP(DotProduct, 0, 0); break;
    case K_VectorAddScaled: FP(VectorAddScaled, (P3*)g.pa, 0, 0, 0); fp.add(g.out, 12, "out"); break;
    case K_APPLY_ENVMAP: FP(APPLY_ENVMAP, in, g.lout); break;
    case K_APPLY_FOG: FP(APPLY_FOG, in, g.lout); break;
    case K_mrModelEnterDeferredMode: FP0(mrModelEnterDeferredMode); break;
    case K_mrModelEnvMap: FP(mrModelEnvMap, 0); break;
    case K_mrModelSetFrame: FP(mrModelSetFrame, g.fr); break;
    case K_mrModelBuild: case K_mrModelBuildLit: if (g.m) FP(mrModelBuild, g.m, info, (int)((uint32_t)g.r[0] % 8)); break;
    case K_direct_model_build: if (g.m) FP(direct_model_build, g.m, info, 0, (int)((uint32_t)g.r[0] % 8)); break;
    case K_mrModelPick: FP(mrModelPick, info, g.fr, 0, 0); break;
    case K_mrModelPickVertex: FP(mrModelPickVertex, info, g.fr, 0, 0, g.outs, g.outs + 1); break;
    case K_mrModelPickEdge: FP(mrModelPickEdge, info, g.fr, 0, 0, g.outs, g.outs + 1); break;
    case K_line_point_dist: FP(line_point_dist, 0, 0, 0, 0); break;
    case K_mr_model_set_frame: FP(mr_model_set_frame, g.fr); break;
    case K_mrModelGetExtents: FP(mrModelGetExtents, 0, (float*)g.outs, (float*)g.outs + 1, (float*)g.outs + 2, (float*)g.outs + 3, (float*)g.outs + 4, (float*)g.outs + 5); break;
    case K_mrModelGetInfo: FP(mrModelGetInfo, 0); break;
    case K_mrModelGetVerts: FP(mrModelGetVerts, 0, (uint8_t**)g.outs, g.outs + 1); break;
    case K_mrModelHasAlpha: FP0(mrModelHasAlpha); break;
    case K_mrModelSetAlpha: FP(mrModelSetAlpha, 0); break;
    case K_mrModelClearAlpha: FP0(mrModelClearAlpha); break;
    case K_mrModelSetClipDist: FP(mrModelSetClipDist, 0, 0); break;
    case K_convert_to_mrmodel: FP(convert_to_mrmodel, 0); break;
    case K_convert_to_mip: FP(convert_to_mip, 0); break;
    case K_begin_deferred_surfs: FP0(begin_deferred_surfs); break;
    case K_add_deferred_surf: if (g.si) FP(add_deferred_surf, 0, 0, g.si); break;
    default: fp.replay_only = "replay-only or a sequence"; break;
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
    if (getenv("VP_FAULTS")) printf("    fault %08lx at %08lx: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx\n",
                                    e->ExceptionRecord->ExceptionCode, (unsigned long)e->ContextRecord->Eip, (unsigned long)e->ContextRecord->Eax,
                                    (unsigned long)e->ContextRecord->Ebx, (unsigned long)e->ContextRecord->Ecx, (unsigned long)e->ContextRecord->Edx,
                                    (unsigned long)e->ContextRecord->Esi, (unsigned long)e->ContextRecord->Edi, (unsigned long)e->ContextRecord->Ebp);
    return EXCEPTION_EXECUTE_HANDLER;
}
static unsigned g_pc;
static bool g_unmask;
static int run_guarded(Kind k, bool rw, uint64_t* ret) {
    g_log.n = 0;
    g_heap = g_heap_mark;
    g_tex_next = 1000;
    g_file_next = 0;
    g_deres_left = g_script.deres_frames;
    unsigned cw;
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    if (g_unmask) _controlfp_s(&cw, _MCW_EM & ~(_EM_OVERFLOW | _EM_ZERODIVIDE), _MCW_EM);
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
    int iterations = argc > 1 ? atoi(argv[1]) : 100000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const char* only = argc > 3 ? argv[3] : 0;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_gx_model.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);         // _amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);         // _NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);         // _FF_MSGBANNER
    patch_jmp(0x004d5c10, (void*)&stub_crt_fatal);         // _fptrap
    patch_jmp(0x004d6850, (void*)&stub_crt_msgbox);        // __crtMessageBoxA
    ((void(__cdecl*)())0x004ce150)();                      // the CRT's floating point, as its start-up leaves it
    struct { uint32_t at; void* to; } stubs[] = {
        {0x004140e0, (void*)&stub_MemAlloc}, {0x00414300, (void*)&stub_MemFree}, {0x00414390, (void*)&stub_delete},
        {0x004112b0, (void*)&stub_panic}, {0x00411150, (void*)&stub_report}, {0x00419fa0, (void*)&stub_ResourceGet},
        {0x0041a450, (void*)&stub_ResourceForget}, {0x0041a5c0, (void*)&stub_ResourceWrite}, {0x004115f0, (void*)&stub_FileCreate},
        {0x00411a30, (void*)&stub_FileWrite}, {0x00411850, (void*)&stub_FileClose},
        {0x00458240, (void*)&stub_dxBegin}, {0x00458260, (void*)&stub_dxEnd}, {0x00458890, (void*)&stub_dxGetCounters},
        {0x00458270, (void*)&stub_dxBegin3D}, {0x004582a0, (void*)&stub_dxEnd3D}, {0x00458450, (void*)&stub_dxRelease},
        {0x00458460, (void*)&stub_dxRestore}, {0x004585b0, (void*)&stub_dxSetViewport}, {0x00458610, (void*)&stub_dxSetViewportColor},
        {0x00458770, (void*)&stub_dxClearZ}, {0x00458800, (void*)&stub_dxProjectPoint}, {0x00458920, (void*)&stub_dxDPBeginFrame},
        {0x00458930, (void*)&stub_dxDPEndFrame}, {0x00458970, (void*)&stub_dxEnableVertexAlpha}, {0x00458990, (void*)&stub_dxDPBegin},
        {0x00458a60, (void*)&stub_dxDPDraw}, {0x00458a90, (void*)&stub_dxDPEnd}, {0x00458aa0, (void*)&stub_dxBeginTriangles},
        {0x00458b70, (void*)&stub_dxDrawTriangle}, {0x00458b90, (void*)&stub_dxDrawTriangles},
        {0x00458bb0, (void*)&stub_dxDrawIndexTriangles}, {0x00458be0, (void*)&stub_dxDrawScreen},
        {0x00458c10, (void*)&stub_dxEndTriangles}, {0x0045de20, (void*)&stub_dxStateInit}, {0x0045def0, (void*)&stub_dxStateClear},
        {0x0045df00, (void*)&stub_dxStateUpdate}, {0x0045dfd0, (void*)&stub_dxStateFlush}, {0x0045f100, (void*)&stub_SetTransform},
        {0x00459f80, (void*)&stub_TextureBegin}, {0x00459fa0, (void*)&stub_TextureEnd}, {0x0045a050, (void*)&stub_TextureReleaseAll},
        {0x0045a090, (void*)&stub_TextureRestoreAll}, {0x0045a410, (void*)&stub_TextureWillDeres},
        {0x0045a420, (void*)&stub_TextureBeginFrame}, {0x0045a4b0, (void*)&stub_TextureEndFrame}, {0x0045a4c0, (void*)&stub_TextureFlush},
        {0x0045a590, (void*)&stub_TextureHasAlpha}, {0x0044e240, (void*)&stub_gxGetTexture}, {0x0044e280, (void*)&stub_gxForgetTexture},
        {0x00455eb0, (void*)&stub_ASSERT_MSG},
    };
    for (auto& st2 : stubs) patch_jmp(st2.at, st2.to);
    *(void**)0x004e642c = (void*)&stub_prof_ob;            // the profiler's function pointers
    *(void**)0x004e6430 = (void*)&stub_prof_oe;
    *(void**)0x004e6434 = (void*)&stub_prof_start;
    *(void**)0x004e6438 = (void*)&stub_prof_stop;
    *(uint32_t*)0x00455707 = 64;                           // the pools, small (M1's operands: the rewrites read them)
    *(uint32_t*)0x00455742 = 32;
    *(uint32_t*)0x00457217 = 256;
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_arena_snap = (uint8_t*)malloc(ARENA_BYTES);
    g_arena_after = (uint8_t*)malloc(ARENA_BYTES);
    g_data_pristine = (uint8_t*)malloc(DATA_BYTES);
    g_data_snap = (uint8_t*)malloc(DATA_BYTES);
    g_data_after = (uint8_t*)malloc(DATA_BYTES);
    memcpy(g_data_pristine, DATA, DATA_BYTES);
    memcpy(g_alpha_prologue, (void*)0x00457340, 5);

    static Footprint fp;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    int unmasked_runs = 0, differ = 0, faults = 0, fault_both = 0, fp_bad = 0, replay_only = 0, wild_runs = 0, wild_differ = 0, setup_bad = 0;
    static int per[N_KINDS], per_bad[N_KINDS], per_fault[N_KINDS];
    // coverage
    int c_proj_fail = 0, c_fresh_v0 = 0, c_panic = 0, c_env2 = 0, c_fog_far = 0, c_overrun = 0, c_draws = 0, c_asserts = 0, c_deres = 0;
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weights[i];
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), kk = 0;
        while (pick >= kind_weights[kk]) pick -= kind_weights[kk++];
        Kind kind = (Kind)kk;
        if (only) {
            kind = N_KINDS;
            for (int i = 0; i < N_KINDS; i++) if (!strcmp(kind_names[i], only)) kind = (Kind)i;
            if (kind == N_KINDS) { printf("no kind %s\n", only); return 2; }
        }
        setup_world();
        if (!g_setup_ok) { setup_bad++; continue; }
        random_statics();
        random_args(kind);
        const bool wild = chance(10) && kind != K_SEQ_LIFTED;
        if (wild) { wildify(); wild_runs++; }
        if (kind == K_SEQ_DEFERRED || kind == K_SEQ_LIFTED)
            for (int q = 0; q < 64; q++)
                g_seq_ids[q] = kind == K_SEQ_LIFTED ? (int)(rnd() % 118) - 1 : chance(85) ? (int)(rnd() % 119) - 1 : (int)(rnd() % 160) - 2;
        if (kind == K_SEQ_LIFTED) {                                  // nothing queued before: the two arrays start alike
            memset((void*)0x00522b40, 0, 0x78 * 4);
            memset(AR(A_LIFT), 0, 1024 * 4);
            seq_lifted_patch(true);
        }
        g_heap_mark = g_heap;
        g_pc = chance(50) ? _PC_53 : _PC_24;
        g_unmask = chance(30);
        if (g_unmask) unmasked_runs++;
        if (trace) printf("world %d: %s\n", it, kind_names[kind]);
        footprint_of(kind, fp);
        if (fp.replay_only) replay_only++;
        memcpy(g_arena_snap, g_arena, ARENA_BYTES);
        memcpy(g_data_snap, DATA, DATA_BYTES);
        uint64_t ro = 0, rn = 0;
        const int fo = run_guarded(kind, false, &ro);
        const uint32_t fo_code = g_fault_code, fo_eip = g_fault_eip;
        memcpy(g_arena_after, g_arena, ARENA_BYTES);
        memcpy(g_data_after, DATA, DATA_BYTES);
        g_log_orig = g_log;
        memcpy(g_arena, g_arena_snap, ARENA_BYTES);
        memcpy(DATA, g_data_snap, DATA_BYTES);
        const int fn = run_guarded(kind, true, &rn);
        if (kind == K_SEQ_LIFTED) { seq_lifted_patch(false); memcpy((void*)0x00457340, g_alpha_prologue, 5); }
        per[kind]++;
        if (fo || fn) {
            faults++;
            per_fault[kind]++;
            if (fo && fn) { fault_both++; if (fault_both <= 5) printf("  (world %d, %s: both faulted, %08x at %08x)\n", it, kind_names[kind], fo_code, fo_eip); }
            if (fo != fn) {
                printf("  world %d (%s): %s / %s; fault %08x at %08x (original's %08x at %08x), address %08x (pc %s, unmasked %d, wild %d)\n",
                       it, kind_names[kind], fo ? "original faulted" : "original ran", fn ? "rewrite faulted" : "rewrite ran", g_fault_code,
                       g_fault_eip, fo_code, fo_eip, g_fault_addr, g_pc == _PC_24 ? "24" : "53", g_unmask, wild);
                differ++; per_bad[kind]++;
                if (differ >= 30) { printf("stopping after 30 differing worlds\n"); break; }
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
            for (uint32_t i = 0; i < m; i++)
                if (g_log.w[i] != g_log_orig.w[i]) {
                    printf("    first at %u: %08x / %08x (before:", i, g_log_orig.w[i], g_log.w[i]);
                    for (uint32_t q = i > 6 ? i - 6 : 0; q < i; q++) printf(" %08x", g_log_orig.w[q]);
                    printf(")\n");
                    break;
                }
            same = false;
        }
        if (!same) {
            differ++; per_bad[kind]++;
            if (wild) wild_differ++;
            printf("    (pc %s, wild %d, exceptions %s)\n", g_pc == _PC_24 ? "24" : "53", wild, g_unmask ? "unmasked" : "masked");
            if (differ >= 30) { printf("stopping after 30 differing worlds\n"); break; }
        }
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
        // coverage, from the original's log and the world
        auto logged = [&](uint32_t tag) { for (uint32_t i = 0; i < g_log_orig.n && i < LOG_MAX; i++) if (g_log_orig.w[i] == tag) return true; return false; };
        if (logged('PANC')) c_panic++;
        if (logged('DXDD') || logged('DXDS')) c_draws++;
        if (logged('ASRT')) c_asserts++;
        if (kind == K_mrModelFlush && g_script.deres_frames) c_deres++;
        if ((kind == K_mrModelPick || kind == K_mrModelPickVertex || kind == K_mrModelPickEdge) && ro == 0xffffffffu) c_proj_fail++;
        if ((kind == K_mrModelInfoGet) && g_script.res_fresh && g_script.res_version == 0 && g_script.res_found) c_fresh_v0++;
        if (*(int32_t*)(g_data_snap + (L_ENV - 0x4e1000)) == 2) c_env2++;
        if (*(uint8_t*)(g_data_snap + (L_FOG - 0x4e1000))) c_fog_far++;
        if (kind == K_SEQ_DEFERRED) for (int q = 0; q < g_args.nseq; q++) if (g_seq_ids[q] >= 119) { c_overrun++; break; }
    }
    printf("%d worlds (%d wild, %d with overflow / divide-by-zero unmasked): %d differ (%d of them wild), %d faulted (%d in both), %d changed bytes outside the footprint, %d replay-only, %d set-ups failed\n",
           iterations, wild_runs, unmasked_runs, differ, wild_differ, faults, fault_both, fp_bad, replay_only, setup_bad);
    printf("per function (worlds / faulted in both / differing):\n");
    for (int i = 0; i < N_KINDS; i++) printf("  %-32s %6d / %5d / %d\n", kind_names[i], per[i], per_fault[i], per_bad[i]);
    printf("coverage: panics %d, draws %d, env-map pass asserts %d, flush de-res frames %d, picks missed %d, fresh v0 .mod %d, env 2 %d, fog on %d, deferred overruns past 118 %d\n",
           c_panic, c_draws, c_asserts, c_deres, c_proj_fail, c_fresh_v0, c_env2, c_fog_far, c_overrun);
    return differ || fp_bad ? 1 : 0;
}
