// world_wld_draw.cpp -- group G1e's rewrites (hook/wld_draw.cpp: the world's drawing -- WorldDraw, the mirror, the 2D,
// the physics triangles, ViewDraw, the track graph, the car, its wheels and x-ray view, the effects' Draw, the models,
// the deleting destructors) against the originals, on random worlds, outside the game (docs/PORTING.md, checking 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_wld_draw.cpp
//        /Fo<dir>\ /Fe<dir>\world_wld_draw.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_wld_draw.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. Every world is
// built in an arena: a CarObject (its LOD rows, models, wheel frames, dust, smoke puffs), up to 64 graphics objects
// with a vtable of loggers (Update / IsVisible / IsAlpha / Draw / Draw2D / GetZ, answers from the object), a shadow,
// a reflection, a smoke puff, skid marks, sparks, a splash, a model object, a random track graph (GrafNodes of every
// type, facing modes 1 / 2 / 4 and a bad one), a canvas; and the image's statics: the camera, the world's switches
// (camera type, focus car, effects, smoke, specular, mirror mode and detail, paused, the physics clock, the alpha
// level), the view's (sky, draw distance, field of view), the graf's LOD and clip, the screen size.
//
// Stubbed (a jump to a logger): the whole renderer below these functions -- mrSetView, mrSetCamera, mrBeginFrame /
// EndFrame, mrModelDraw (both), mrModelBuild (logs the model it builds), alpha, clip, projection, features,
// blend, push / pop, the triangle calls (log the vertices, without +0xc, and without u / v where the original leaves
// them unwritten), deferred mode, camera position / distance, sphere test, lighting, env map, the x-ray's
// GetVerts / GetInfo (arena buffers), mrProjectPoint, mrMirrorView --, gxLine, gxSetCanvas, gxTextCentered,
// gxDrawStamp, the profiler, PTimeNow (a scripted clock), TrackDraw, the terrain (scripted heights and hits),
// CarMgrGetInfo, MultiEnabled, HackShowCarStatusInfo, sprintf (the host's), LogPanic, Random (scripted), operator
// delete, MemFree, find_available_smoke (scripted), SmokeObject::Puff, gxForgetTexture and the destructors under the
// deleting destructors. Everything else runs as the game's code: the getters, the matrix and vector helpers,
// interpolate, footensor, both GetParticlePosition, RandomReal, memmove, SkidObject::Clear, the mirror mode, and this
// group's own functions where another calls them (WorldDraw -> ViewDraw / draw_physics_tris / draw_mirror,
// CarObject::Draw -> DrawWheel / DrawXRay, GrafDraw / draw_tree -> draw_tree).
//
// Each world picks a function; the arena and the image's whole .data/.bss are saved, the original runs, the result is
// kept, the world restored, the rewrite runs; the arena, .data/.bss, the return value and the stubs' logs are
// compared, and every byte the original changed must lie in the rewrite's footprint unless it's replay_only. The FPU
// runs at a random precision (24 or 53 bits), in 30% of the worlds with overflow and divide-by-zero unmasked. A share
// of the worlds has floats replaced by extreme values (never the pointers).
// Debugging: VP_TRACE=1 names each world's function; VP_DUMP=<world> prints both passes' logs side by side (and the
// camera and upright graf nodes) if that world differs; VP_XPRD=1 replaces CrossProduct by a logging copy (and
// VP_XPRD2=1 prints the caller's stack at each call, for GrafDraw's locals).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VP_FAITHFUL
#include "../hook/port.h"
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { printf("footprint: object() not expected (%s)\n", what); add(obj, 4, what); }
void logf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

#include "../hook/wld_draw.cpp"

// ---- random numbers --------------------------------------------------------------------------------------
static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float range(float a, float b) { return a + (b - a) * uni(); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
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
static void setf(void* p, float f) { memcpy(p, &f, 4); }
static void rnd_p3(void* p, float lo, float hi) { float* f = (float*)p; for (int i = 0; i < 3; i++) f[i] = range(lo, hi); }
static void random_rot(float* m) {                                  // roughly a rotation
    const float a = range(-3.2f, 3.2f), b = range(-0.5f, 0.5f);
    const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
    const float r[9] = {ca, 0, -sa, sa * sb, cb, ca * sb, sa * cb, -sb, ca * cb};
    for (int i = 0; i < 9; i++) m[i] = r[i] + (chance(10) ? range(-0.01f, 0.01f) : 0.0f);
}
static void random_frame(float* f, float spread) { random_rot(f); rnd_p3(f + 9, -spread, spread); }

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
    ARENA_BYTES = 0x20000,
    A_CAR = 0x0000,                          // CarObject (0x844)
    A_LODROWS = 0x0900,                      // 8 LodRows
    A_LODMODELS = 0x0980,                    // 8 model ids
    A_CAMPOS = 0x09c0,                       // mrGetCameraPos's point
    A_GOBS = 0x1000,                         // 64 graphics objects, 0x40 each
    A_GOBVT = 0x2000,                        // their vtable
    A_SHADOW = 0x2100, A_WOBJ = 0x2200,      // a ShadowObject, its WorldObject (frame at +4)
    A_SHINFO = 0x2600, A_SHCOPY = 0x3000,    // the shadow's model and its copy (info, verts at +0x100)
    A_REFL = 0x3a00, A_SMOKE = 0x3a40, A_SKID = 0x3b00, A_MARKS = 0x3c00,
    A_SPARKS = 0x4600, A_SPLASH = 0x4a00, A_MODEL = 0x4e00, A_DDOBJ = 0x4f00,
    A_GRAF = 0x5000,                         // GrafNodes, 0x50 each (up to 120)
    A_XVERTS = 0x8000,                       // 4 vertex buffers of 32 (the x-ray's arm copies and sources)
    A_XINFO = 0x9000,                        // their mrModelInfos
    A_CARINFO = 0x9200, A_CANVAS = 0x9400, A_PIX = 0x9800, A_SMOKEOBJS = 0xa000, A_STAMP = 0xa400,
    A_END = 0xa500,
};
static uint8_t* g_arena;
static uint8_t* g_arena_snap;
static uint8_t* g_arena_after;
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* g_data_snap;
static uint8_t* g_data_after;
static uint8_t* AR(uint32_t off) { return g_arena + off; }
static bool in_arena(const void* p, uint32_t n) { return (const uint8_t*)p >= g_arena && (const uint8_t*)p + n <= g_arena + ARENA_BYTES && n < ARENA_BYTES; }
static uint32_t rel(const void* p) { return in_arena(p, 1) ? 0xa0000000u + (uint32_t)((const uint8_t*)p - g_arena) : (uint32_t)(uintptr_t)p; }

// ---- the stubs' log and script ------------------------------------------------------------------------------------
enum { LOG_MAX = 32768 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void log_word(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static void log_bytes(const void* p, int n) { for (int i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, (const uint8_t*)p + i, n - i < 4 ? n - i : 4); log_word(w); } }
static void log_str(const char* s) { size_t n = strlen(s); log_word((uint32_t)n); log_bytes(s, (int)n); }
static bool g_log_uv = true;                        // the vertices' u / v (not written by draw_physics_tris)
static void log_vtx(const void* v) {
    const uint32_t* w = (const uint32_t*)v;
    log_word(w[0]); log_word(w[1]); log_word(w[2]); log_word(w[4]); log_word(w[5]);
    if (g_log_uv) { log_word(w[6]); log_word(w[7]); }
}
static void log_frame(const void* f) { if (f) log_bytes(f, 0x30); else log_word(0xdead); }

struct Script { uint32_t seed; int xn[4]; };
static Script g_script;
static uint32_t g_rand;
static uint32_t next() { g_rand ^= g_rand << 13; g_rand ^= g_rand >> 17; g_rand ^= g_rand << 5; return g_rand; }
static float next_range(float a, float b) { return a + (b - a) * ((float)(next() & 0xffffff) / 16777216.0f); }
static int g_time;

// the renderer
static void __cdecl s_SetView(int x, int y, int w, int h, uint8_t t) { log_word('SVIW'); log_word(x); log_word(y); log_word(w); log_word(h); log_word(t); }
static void __cdecl s_SetCamera(const void* f) { log_word('SCAM'); log_frame(f); }
static void __cdecl s_FinishDeferred() { log_word('FDEF'); }
static void __cdecl s_EndFrame() { log_word('EFRM'); }
static void __cdecl s_BeginFrame() { log_word('BFRM'); }
static void __cdecl s_ModelDraw(int m, const void* f) { log_word('MDRW'); log_word(m); log_frame(f); }
static void __cdecl s_ModelDrawI(int m) { log_word('MDRI'); log_word(m); }
static void __cdecl s_ModelBuild(int m, const uint8_t* info, int flags) {
    log_word('MBLD'); log_word(m); log_word(rel(info)); log_word(flags);
    if (!in_arena(info, 0x28)) return;
    const int32_t n = *(const int32_t*)info;
    const uint8_t* v = *(uint8_t* const*)(info + 4);
    if (n > 0 && n <= 64 && in_arena(v, n * 32)) log_bytes(v, n * 32);
}
static void __cdecl s_SetAlpha(uint32_t a) { log_word('SALP'); log_word(a); }
static void __cdecl s_ClearAlpha() { log_word('CALP'); }
static void __cdecl s_Enable(int f) { log_word('ENAB'); log_word(f); }
static void __cdecl s_Disable(int f) { log_word('DISA'); log_word(f); }
static uint8_t __cdecl s_IsEnabled(int f) { uint8_t r = (uint8_t)(next() % 3 == 0); log_word('ISEN'); log_word(f); log_word(r); return r; }
static void __cdecl s_SetProjection(uint32_t a, uint32_t b, uint32_t c) { log_word('SPRJ'); log_word(a); log_word(b); log_word(c); }
static void __cdecl s_SetClipDist(uint32_t a, uint32_t b) { log_word('SCLP'); log_word(a); log_word(b); }
static void __cdecl s_BeginTriangles(int t) { log_word('BTRI'); log_word(t); }
static void __cdecl s_DrawTriangle(const uint8_t* v) { log_word('DTRI'); for (int k = 0; k < 3; k++) log_vtx(v + 32 * k); }
static void __cdecl s_EndTriangles() { log_word('ETRI'); }
static void __cdecl s_DrawIndexTriangles(const uint8_t* v, int nv, const uint16_t* idx, int ni) {
    log_word('DITR'); log_word(nv); log_word(ni);
    for (int k = 0; k < nv && k < 64; k++) log_vtx(v + 32 * k);
    for (int k = 0; k < ni && k < 128; k++) log_word(idx[k]);
}
static void __cdecl s_SetFrame(const void* f) { log_word('SFRM'); log_word(rel(f)); log_frame(f); }
static void __cdecl s_EnterDeferred() { log_word('EDEF'); }
static void __cdecl s_LeaveDeferred() { log_word('LDEF'); }
static void* __cdecl s_GetCameraPos() { log_word('GCPS'); return AR(A_CAMPOS); }
static float __cdecl s_CameraDistanceSqr(const float* p) {
    log_word('CDSQ'); log_bytes(p, 12);
    const uint32_t r = next();
    return r % 16 == 0 ? bitsf(0x7fc00000) : next_range(0.0f, 2000.0f);
}
static uint8_t __cdecl s_SphereCanSee(const float* p, uint32_t rad) { log_word('SPHR'); log_bytes(p, 12); log_word(rad); return (uint8_t)(next() % 4 != 0); }
static void __cdecl s_SetBlendMode(int m) { log_word('BLND'); log_word(m); }
static void __cdecl s_PushState() { log_word('PUSH'); }
static void __cdecl s_PopState() { log_word('POP_'); }
static void __cdecl s_LightSpecial(int i, const void* f) { log_word('LSPC'); log_word(i); log_word(rel(f)); }
static void __cdecl s_EnvMap(int b) { log_word('ENVM'); log_word(b); }
static int xslot(int m) { return m >= 0x301 && m <= 0x304 ? m - 0x301 : -1; }
static void __cdecl s_GetVerts(int m, uint8_t** v, int* n) {
    log_word('GVRT'); log_word(m);
    const int k = xslot(m);
    *v = AR(A_XVERTS) + 0x400 * (k < 0 ? 0 : k);
    *n = k < 0 ? 0 : g_script.xn[k];
}
static void* __cdecl s_GetInfo(int m) { log_word('GINF'); log_word(m); const int k = xslot(m); return AR(A_XINFO) + 0x28 * (k < 0 ? 0 : k); }
static uint8_t __cdecl s_ProjectPoint(const float* p, float* q) {
    log_word('PROJ'); log_bytes(p, 12);
    q[0] = next_range(-50.0f, 700.0f); q[1] = next_range(-50.0f, 500.0f); q[2] = next_range(0.0f, 1.0f);
    return (uint8_t)(next() % 5 != 0);
}
static void __cdecl s_MirrorView(uint8_t b) { log_word('MIRV'); log_word(b); }
// 2D
static void __cdecl s_Line(int a, int b, int c, int d, uint32_t col) { log_word('LINE'); log_word(a); log_word(b); log_word(c); log_word(d); log_word(col); }
static void* __cdecl s_SetCanvas(void* c) { log_word('SCNV'); log_word(rel(c)); *(uint32_t*)0x004efbf8 = (uint32_t)(uintptr_t)c; return 0; }
static void __cdecl s_TextCentered(int x, int y, const char* s, uint32_t col) { log_word('TXTC'); log_word(x); log_word(y); log_str(s); log_word(col); }
static void __cdecl s_DrawStamp(const void* st, int x, int y, int a, int b) { log_word('STMP'); log_word(rel(st)); log_word(x); log_word(y); log_word(a); log_word(b); }
// misc
static void __cdecl s_SingleEnter(int a, const char*, int) { log_word('SENT'); log_word(a); }
static void __cdecl s_SingleLeave(int a, const char*, int) { log_word('SLEV'); log_word(a); }
static int __cdecl s_PTimeNow() { g_time += 1 + (int)(next() % 5); log_word('TIME'); return g_time; }
static void __cdecl s_TrackDraw() { log_word('TRCK'); }
static uint8_t __cdecl s_TerrainGetHeight(float* p) {
    log_word('THGT'); log_word(fb(p[0])); log_word(fb(p[2]));
    if (next() % 8 == 0) { log_word(0); return 0; }
    p[1] = next_range(-3.0f, 2.0f);
    log_word(fb(p[1]));
    return 1;
}
static uint8_t __cdecl s_Isect4(const float* a, const float* b, float* hit, float* nrm) {
    log_word('ISC4'); log_bytes(a, 12); log_bytes(b, 12);
    for (int i = 0; i < 3; i++) hit[i] = next_range(-300.0f, 300.0f);
    for (int i = 0; i < 3; i++) nrm[i] = next_range(-1.0f, 1.0f);
    return (uint8_t)(next() % 6 != 0);
}
static uint8_t __cdecl s_Isect5(const float* a, const float* b, float* hit, float* nrm, int32_t* surf) {
    log_word('ISC5'); log_bytes(a, 12); log_bytes(b, 12);
    for (int i = 0; i < 3; i++) hit[i] = a[i] + next_range(-2.0f, 2.0f);
    for (int i = 0; i < 3; i++) nrm[i] = next_range(-1.0f, 1.0f);
    *surf = (int32_t)(next() % 16);
    return (uint8_t)(next() % 6 != 0);
}
static void* __cdecl s_CarMgrGetInfo(int i) { log_word('CMGI'); log_word(i); return AR(A_CARINFO); }
static uint8_t __cdecl s_MultiEnabled() { uint8_t r = (uint8_t)(next() % 2); log_word('MULT'); log_word(r); return r; }
static uint8_t __cdecl s_HackShow() { uint8_t r = (uint8_t)(next() % 2); log_word('HACK'); log_word(r); return r; }
static int __cdecl s_sprintf(char* buf, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    log_word('SPRF'); log_word((uint32_t)(uintptr_t)fmt); log_str(buf);
    return r;
}
static void __cdecl s_panic(const char* fmt, ...) { log_word('PANC'); log_word((uint32_t)(uintptr_t)fmt); }
static int __cdecl s_Random(int n) {
    int v = 0;
    if (n > 0) { uint32_t k = next() % 64; v = k == 0 ? 0 : k == 1 ? n - 1 : (int)(next() % (uint32_t)n); }
    log_word('RAND'); log_word((uint32_t)n); log_word((uint32_t)v);
    return v;
}
static void __cdecl s_delete(void* p) { log_word('DELE'); log_word(rel(p)); }
static void __cdecl s_MemFree(void* p) { log_word('MFRE'); log_word(rel(p)); }
static int __fastcall s_FindSmoke(uint8_t* car, int) {
    const int n = *(int32_t*)(car + 0x838);
    const int r = n <= 0 || next() % 4 == 0 ? -1 : (int)(next() % (uint32_t)n);
    log_word('FSMK'); log_word(rel(car)); log_word((uint32_t)r);
    return r;
}
static void __fastcall s_Puff(void* self, int, const float* p, const float* v, uint8_t flag) {
    log_word('PUFF'); log_word(rel(self)); log_bytes(p, 12); log_bytes(v, 12); log_word(flag);
}
static void __cdecl s_ForgetTexture(int t) { log_word('FTEX'); log_word(t); }
static void __cdecl s_Cross(float* o, const float* a, const float* b) {
    log_word('XPRD'); log_bytes(a, 12); log_bytes(b, 12);
    if (getenv("VP_XPRD2")) { const uint32_t* w = (const uint32_t*)b - 3; for (int i = 0; i < 20; i++) printf("      stack+%02x %08x %.9g\n", 0x10 + 4 * i, w[i], bitsf(w[i])); }
    const float x = (float)((double)a[1] * b[2] - (double)b[1] * a[2]);
    const float y = (float)((double)b[0] * a[2] - (double)a[0] * b[2]);
    const float z = (float)((double)a[0] * b[1] - (double)b[0] * a[1]);
    o[0] = x; o[1] = y; o[2] = z;
}
static void __fastcall s_dtor(void* self, int) { log_word('DTOR'); log_word(rel(self)); log_word(*(uint32_t*)self); }
// the graphics objects' virtuals (the answers are the object's +8 visible, +9 alpha, +0xc z)
static void __fastcall g_Update(uint8_t* s, int) { log_word('GUPD'); log_word(rel(s)); }
static uint8_t __fastcall g_IsVisible(uint8_t* s, int) { log_word('GVIS'); log_word(rel(s)); return s[8]; }
static uint8_t __fastcall g_IsAlpha(uint8_t* s, int) { log_word('GALP'); log_word(rel(s)); return s[9]; }
static void __fastcall g_Draw(uint8_t* s, int) { log_word('GDRW'); log_word(rel(s)); }
static void __fastcall g_Draw2D(uint8_t* s, int, void* c) { log_word('GD2D'); log_word(rel(s)); log_word(rel(c)); }
static float __fastcall g_GetZ(uint8_t* s, int) { log_word('GGTZ'); log_word(rel(s)); return *(float*)(s + 0xc); }

// the game's CRT must never put up a modal box
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }
static int __cdecl stub_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}

// ---- the image's statics --------------------------------------------------------------------------------------
enum : uint32_t {
    X_TICK = 0x0052161c, X_FX = 0x0055404c, X_ALPHA = 0x00522a1c, X_SMOKE = 0x0055336c, X_SPEC = 0x00553320,
    X_FOCUS = 0x004f4358, X_CAMTYPE = 0x0055335c, X_MIRROR = 0x004f4c9c, X_PAUSED = 0x00521608, X_CAMERA = 0x00553328,
    X_MIRROR_DETAIL = 0x005522b0, X_MIRROR_X = 0x004f4364, X_MIRROR_COLOR = 0x00554050, X_PHYS_TRIS = 0x004f436c,
    X_GOBS = 0x005522d8, X_NUM_GOBS = 0x00553368, X_WID = 0x005228f4, X_HIT = 0x005228d4, X_DRAW_DIST = 0x00555864,
    X_SKY_ON = 0x00555874, X_FOV = 0x005558ac, X_SKY_MODEL = 0x00555898, X_FIRST_TIME = 0x005571f0, X_TAG = 0x0055803c,
    X_MIN_LOD = 0x004f5284, X_LOD_BIAS = 0x004f5290, X_CLIP = 0x004f528c, X_GRAF_ROOT = 0x0055997c,
    X_FACING_N = 0x00558108, X_UPRIGHT_N = 0x00559998, X_LODB = 0x0055586c,
};
static float now_time() { return (float)(*(int32_t*)X_TICK * (double)bitsf(0x3c83126f)); }

enum Kind {
    K_WORLD_DRAW, K_DRAW_MIRROR, K_WORLD_DRAW2D, K_PHYS_TRIS, K_VIEW_DRAW, K_VIEW_DRAW_MIRROR, K_DRAW_TREE, K_GRAF_DRAW,
    K_CAR_DRAW, K_CAR_DRAW2D, K_CAR_WHEEL, K_CAR_XRAY, K_SHADOW, K_SMOKE, K_REFL, K_SKID, K_MODEL, K_WOBBLE, K_DEBRIS,
    K_SPARKS, K_SPLASH, K_DD_CAR, K_DD_GHOST, K_DD_SHADOW, K_DD_SMOKE, K_DD_REFL, K_DD_SKID, K_DD_MODEL, K_DD_WOBBLE,
    K_DD_SPLASH, K_DD_DEBRIS, K_DD_SPARKS, N_KINDS
};
static const char* kind_names[N_KINDS] = {
    "WorldDraw", "draw_mirror", "WorldDraw2D", "draw_physics_tris", "ViewDraw", "ViewDrawMirror", "draw_tree", "GrafDraw",
    "CarObject::Draw", "CarObject::Draw2D", "CarObject::DrawWheel", "CarObject::DrawXRay", "ShadowObject::Draw",
    "SmokeObject::Draw", "ReflectionObject::Draw", "SkidObject::Draw", "ModelObject::Draw", "WobbleObject::Draw",
    "DebrisObject::Draw", "SparksObject::Draw", "SplashObject::Draw", "CarObject sdd", "GhostCarObject sdd",
    "ShadowObject sdd", "SmokeObject vdd", "ReflectionObject vdd", "SkidObject vdd", "ModelObject sdd", "WobbleObject sdd",
    "SplashObject vdd", "DebrisObject vdd", "SparksObject vdd",
};
static int kind_weight(Kind k) {
    switch (k) {
    case K_CAR_DRAW: case K_CAR_WHEEL: case K_SHADOW: case K_GRAF_DRAW: case K_DRAW_TREE: return 60;
    case K_WORLD_DRAW: case K_CAR_XRAY: case K_REFL: case K_SPARKS: case K_SKID: case K_CAR_DRAW2D: case K_DRAW_MIRROR: return 40;
    default: return k >= K_DD_CAR ? 5 : 20;
    }
}

struct Args { int i; uint8_t b; uint32_t u; };
static Args g_args;

// ---- building the world -----------------------------------------------------------------------------------------
static int g_graf_nodes;
static uint8_t* graf_tree(int depth) {                              // a sibling list, children below
    const int n = 1 + (int)(rnd() % (depth > 2 ? 3 : 5));
    uint8_t* first = 0;
    uint8_t* prev = 0;
    for (int i = 0; i < n && g_graf_nodes < 120; i++) {
        uint8_t* g = AR(A_GRAF) + 0x50 * g_graf_nodes++;
        memset(g, 0, 0x50);
        const int t = (int)(rnd() % 10);
        const int type = t < 2 ? 0 : t < 4 ? 1 : t < 6 ? 2 : t < 8 ? 3 : 4;
        *(int32_t*)g = chance(2) ? 7 : type;
        *(int32_t*)(g + 0xc) = chance(15) ? -1 : 0x500 + (int)(rnd() % 100);
        if (type == 1) g[0xc] = (uint8_t)chance(60);
        rnd_p3(g + 0x10, -500.0f, 500.0f);
        setf(g + 0x18, range(0.0f, 2000.0f));
        setf(g + 0x1c, chance(15) ? -1.0f : range(0.0f, 800.0f));
        if (chance(3)) setf(g + 0x18, wildf());
        const int m = (int)(rnd() % 10);
        *(int32_t*)(g + 0x38) = m < 4 ? 1 : m < 8 ? 2 : m < 9 ? 4 : 3;
        rnd_p3(g + 0x3c, -300.0f, 300.0f);
        if (chance(5)) { memcpy(g + 0x3c, AR(A_CAMPOS), 12); }
        if (depth < 4 && type <= 2 && chance(70)) *(uint8_t**)(g + 8) = graf_tree(depth + 1);
        if (prev) *(uint8_t**)(prev + 4) = g; else first = g;
        prev = g;
    }
    return first;
}

static void random_car() {
    CarObject* c = (CarObject*)AR(A_CAR);
    uint8_t* raw = (uint8_t*)c;
    for (int k = 0; k < 0x844; k += 4) setf(raw + k, range(-2.0f, 2.0f));
    c->vtable = (void**)0x004dd4d0;
    random_frame((float*)&c->frame, 300.0f);
    // the message: wheels
    for (int w = 0; w < 4; w++) {
        WheelMsg* m = wheel_msg(c, w);
        rnd_p3(&m->hub_pos, -1.0f, 1.0f);
        m->steer_angle = range(-0.6f, 0.6f);
        m->omega = chance(50) ? range(-40.0f, 40.0f) : range(-100.0f, 100.0f);
        m->spin_angle = range(-10.0f, 10.0f);
        m->brake_temp = chance(50) ? range(800.0f, 1100.0f) : range(0.0f, 900.0f);
        m->fx_flags = (uint8_t)rnd();
    }
    setf(c->msg + 0x40, chance(50) ? range(0.0f, 0.02f) : range(0.0f, 1.0f));   // +0x7c braking
    opacity(c) = chance(70) ? 1.0f : range(0.0f, 1.2f);
    c->index = (int32_t)(rnd() % 4);
    for (int k = 0; k < 8; k++) {
        LodRow* r = (LodRow*)AR(A_LODROWS) + k;
        r->dist = range(0.0f, 100.0f);
        const int f = (int)(rnd() % 10);
        r->faces = f < 2 ? -1 : f < 9 ? (int)(rnd() % 3) : (int)(rnd() % 6);
        r->spec = (uint8_t)chance(50);
        r->alpha = (uint8_t)chance(30);
        ((int32_t*)AR(A_LODMODELS))[k] = 0x100 + k;
    }
    c->lod_p = (LodRow*)AR(A_LODROWS);
    c->lod_models_p = (int32_t*)AR(A_LODMODELS);
    c->lod_now = (int32_t)(rnd() % 8);
    c->lod_now2 = (int32_t)(rnd() % 8);
    c->shadow_model = chance(50) ? 0 : 0x120;
    c->needle[0] = 0x130; c->needle[1] = 0x131;
    c->has_cockpit = (uint8_t)chance(70);
    c->cockpit_c = 0x132; c->cockpit_w = 0x133;
    for (int k = 0; k < 4; k++) c->arm[k] = 0x140 + k;
    c->arm[4] = 0x303; c->arm[5] = 0x304;
    c->arm_copy[0] = 0x301; c->arm_copy[1] = 0x302;
    c->diskglow = 0x150; c->spin[0] = 0x151; c->spin[1] = 0x152; c->brakelight = 0x153;
    c->needle1_min = range(-3.0f, 0.0f); c->needle1_scale = range(-0.01f, 0.0f);
    c->needle2_min = range(-3.0f, 0.0f); c->needle2_scale = range(-0.01f, 0.0f);
    c->effects_tex = 0x77;
    const float now = now_time();
    for (int k = 0; k < 16; k++) {
        DustParticle* p = &c->dust[k];
        rnd_p3(&p->pos, -2.0f, 2.0f);
        rnd_p3(&p->vel, -300.0f, 300.0f);
        const int r = (int)(rnd() % 10);
        p->life = r < 2 ? 1e6f : r < 8 ? now - range(-0.5f, 2.5f) : now + range(0.0f, 1.0f);
    }
    c->dust_next = (int32_t)(rnd() % 16);
    const int r = (int)(rnd() % 10);
    c->dust_time = r < 3 ? now : r < 6 ? now - range(0.0f, 0.2f) : r < 8 ? now + range(0.0f, 1.0f) : 0.0f;
    c->cam_dist = range(0.0f, 200.0f);
    for (int k = 0; k < 3; k++) { c->wheel_models[k].fwheel_front = 0x160 + 4 * k; c->wheel_models[k].wheel_front = 0x161 + 4 * k;
                                  c->wheel_models[k].fwheel_rear = 0x162 + 4 * k; c->wheel_models[k].wheel_rear = 0x163 + 4 * k; }
    for (int w = 0; w < 4; w++) { random_frame((float*)&c->wheel_frame[w], 300.0f); rnd_p3(&c->wheel_move[w], -0.5f, 0.5f); }
    c->radius_front = range(0.25f, 0.4f);
    c->stamp = AR(A_STAMP);
    c->num_smoke = chance(10) ? 0 : 1 + (int)(rnd() % 8);
    for (int k = 0; k < 32; k++) c->smoke[k] = k < 8 ? AR(A_SMOKEOBJS) + 0x60 * k : 0;
    for (int k = 0; k < 8; k++) *(uint8_t**)(AR(A_SMOKEOBJS) + 0x60 * k + 0x34) = 0;
    // the x-ray's buffers
    for (int k = 0; k < 4; k++) {
        g_script.xn[k] = chance(10) ? 0 : 1 + (int)(rnd() % 32);
        for (int v = 0; v < 32; v++) {
            float* p = (float*)(AR(A_XVERTS) + 0x400 * k + 32 * v);
            rnd_p3(p, -0.3f, 0.3f);
            if (chance(3)) setf(&p[1], 0.1f);
        }
        uint8_t* info = AR(A_XINFO) + 0x28 * k;
        memset(info, 0, 0x28);
        *(int32_t*)info = g_script.xn[k];
        *(uint8_t**)(info + 4) = AR(A_XVERTS) + 0x400 * k;
    }
    // the car manager's info
    uint8_t* ci = AR(A_CARINFO);
    memset(ci, 0, 0x200);
    const char* names[4] = {"Viper", "Joe", "a much longer driver name", ""};
    strcpy((char*)ci + 5, names[rnd() % 4]);
    *(int32_t*)(ci + 0x140) = (int32_t)(rnd() % 100);
    strcpy((char*)ci + 0x32, chance(50) ? "" : "thinking");
    // the canvas
    uint8_t* cv = AR(A_CANVAS);
    memset(cv, 0, 0x24);
    cv[0] = 4;
    *(uint8_t**)(cv + 4) = AR(A_PIX);
    *(int32_t*)(cv + 8) = 32; *(int32_t*)(cv + 0xc) = 16; *(int32_t*)(cv + 0x10) = 64;
}

static void random_world() {
    memset(g_arena, 0, ARENA_BYTES);
    g_script.seed = rnd() | 1;
    const int32_t tick = chance(10) ? (int32_t)(rnd() % 100) : (int32_t)(rnd() % 200000);
    *(int32_t*)X_TICK = tick;
    const float now = now_time();
    *(uint8_t*)X_FX = chance(85) ? 1 : (uint8_t)(chance(50) ? 0 : rnd());
    *(int32_t*)X_ALPHA = (int32_t)(rnd() % 3);
    *(int32_t*)X_SMOKE = chance(85) ? 1 : (int32_t)(rnd() % 3);
    *(int32_t*)X_SPEC = (int32_t)(rnd() % 3);
    *(int32_t*)X_FOCUS = (int32_t)(rnd() % 4);
    *(int32_t*)X_CAMTYPE = chance(10) ? 11 : (int32_t)(rnd() % 6) - 1;
    *(uint8_t*)X_MIRROR = (uint8_t)chance(15);
    *(uint8_t*)X_PAUSED = (uint8_t)chance(15);
    *(int32_t*)X_MIRROR_DETAIL = (int32_t)(rnd() % 4);
    *(int32_t*)X_MIRROR_X = 0x100 + (int32_t)(rnd() % 64);
    *(uint32_t*)X_MIRROR_COLOR = rnd();
    *(uint8_t*)X_PHYS_TRIS = (uint8_t)chance(50);
    *(int32_t*)X_WID = 640; *(int32_t*)X_HIT = 480;
    setf((void*)X_DRAW_DIST, range(100.0f, 1500.0f));
    *(uint8_t*)X_SKY_ON = (uint8_t)chance(70);
    setf((void*)X_FOV, range(0.8f, 1.4f));
    *(int32_t*)X_SKY_MODEL = 0x90;
    *(int32_t*)X_FIRST_TIME = chance(50) ? 0 : 1234;
    *(uint32_t*)X_TAG = rnd();
    setf((void*)X_MIN_LOD, range(0.0f, 100.0f));
    setf((void*)X_LOD_BIAS, range(0.5f, 2.0f));
    setf((void*)X_CLIP, chance(80) ? 90000.0f : range(0.0f, 200000.0f));
    setf((void*)X_LODB, range(0.5f, 2.0f));
    float* cam = (float*)X_CAMERA;
    random_frame(cam, 300.0f);
    rnd_p3(AR(A_CAMPOS), -300.0f, 300.0f);
    // the graphics objects
    void** vt = (void**)AR(A_GOBVT);
    for (int i = 0; i < 16; i++) vt[i] = (void*)0x00000bad;
    vt[2] = (void*)&g_Update; vt[3] = (void*)&g_IsVisible; vt[4] = (void*)&g_IsAlpha; vt[5] = (void*)&g_Draw;
    vt[6] = (void*)&g_Draw2D; vt[8] = (void*)&g_GetZ;
    const int ngobs = chance(5) ? 0 : (int)(rnd() % 64);
    *(int32_t*)X_NUM_GOBS = ngobs;
    for (int i = 0; i < ngobs; i++) {
        uint8_t* g = AR(A_GOBS) + 0x40 * i;
        *(void***)g = vt;
        g[8] = (uint8_t)chance(80);
        g[9] = (uint8_t)chance(40);
        setf(g + 0xc, chance(10) ? (float)(rnd() % 3) : chance(3) ? wildf() : range(-100.0f, 100.0f));
        ((uint8_t**)X_GOBS)[i] = g;
    }
    random_car();
    if (chance(50)) {                                               // the car within the name tag's 100 m
        CarObject* c = (CarObject*)AR(A_CAR);
        c->frame.pos.x = cam[9] + range(-60.0f, 60.0f);
        c->frame.pos.y = cam[10] + range(-10.0f, 10.0f);
        c->frame.pos.z = cam[11] + range(-60.0f, 60.0f);
    }
    // the shadow and its car
    uint8_t* w = AR(A_WOBJ);
    random_frame((float*)(w + 4), 300.0f);
    if (chance(20)) { memcpy(w + 0x28, cam + 9, 12); }
    uint8_t* sh = AR(A_SHADOW);
    *(uint32_t*)sh = 0x004dd258;
    *(uint8_t**)(sh + 4) = w;
    setf(sh + 0xc, range(0.0f, 12000.0f));
    *(uint8_t**)(sh + 0x10) = chance(95) ? AR(A_SHINFO) : 0;
    *(uint8_t**)(sh + 0x14) = AR(A_SHCOPY);
    *(int32_t*)(sh + 0x18) = 0x60;
    const int nv = chance(5) ? 0 : 1 + (int)(rnd() % 40);
    *(int32_t*)AR(A_SHINFO) = nv;
    *(uint8_t**)(AR(A_SHINFO) + 4) = AR(A_SHINFO) + 0x100;
    *(int32_t*)AR(A_SHCOPY) = nv;
    *(uint8_t**)(AR(A_SHCOPY) + 4) = AR(A_SHCOPY) + 0x100;
    for (int k = 0; k < 40; k++) {
        float* v = (float*)(AR(A_SHINFO) + 0x100 + 32 * k);
        v[0] = range(-1.0f, 1.0f); v[1] = range(-0.5f, 1.5f); v[2] = range(-2.0f, 2.0f);
        for (int j = 3; j < 8; j++) v[j] = range(-1.0f, 1.0f);
        float* cvv = (float*)(AR(A_SHCOPY) + 0x100 + 32 * k);
        for (int j = 0; j < 8; j++) cvv[j] = range(-1.0f, 1.0f);
    }
    // the reflection
    uint8_t* rf = AR(A_REFL);
    *(uint32_t*)rf = 0x004dd2f8;
    *(uint8_t**)(rf + 4) = w;
    setf(rf + 8, range(0.0f, 1000.0f));
    *(int32_t*)(rf + 0xc) = 0x61;
    rnd_p3(rf + 0x10, -1.0f, 1.0f);
    // the smoke puff
    uint8_t* sm = AR(A_SMOKE);
    *(uint32_t*)sm = 0x004dd2a8;
    random_frame((float*)(sm + 4), 300.0f);
    *(int32_t*)(sm + 0x38) = 0x62;
    setf(sm + 0x54, range(0.0f, 1.0f));
    // the skid marks
    uint8_t* sk = AR(A_SKID);
    *(uint32_t*)sk = 0x004dd320;
    *(uint8_t**)(sk + 4) = AR(A_MARKS);
    *(int32_t*)(sk + 8) = chance(5) ? 0 : (int)(rnd() % 40);
    *(int32_t*)(sk + 0xc) = (int32_t)(rnd() % 40);
    *(int32_t*)(sk + 0x10) = 0x63;
    for (int k = 0; k < 40; k++) {
        uint8_t* m = AR(A_MARKS) + 0x38 * k;
        for (int j = 0; j < 12; j++) setf(m + 4 * j, range(-300.0f, 300.0f));
        *(int32_t*)(m + 0x30) = (int32_t)(rnd() % 3);
        *(int32_t*)(m + 0x34) = (int32_t)(rnd() % 8);
    }
    // the sparks
    uint8_t* sp = AR(A_SPARKS);
    *(uint32_t*)sp = 0x004dd5b0;
    for (int k = 0; k < 32; k++) {
        uint8_t* p = sp + 4 + 0x1c * k;
        rnd_p3(p, -10.0f, 10.0f);
        rnd_p3(p + 0xc, -300.0f, 300.0f);
        const int r = (int)(rnd() % 10);
        setf(p + 0x18, r < 2 ? 1e6f : r < 8 ? now - range(-0.3f, 1.3f) : now + range(0.0f, 0.5f));
    }
    *(int32_t*)(sp + 0x384) = (int32_t)(rnd() % 32);
    *(int32_t*)(sp + 0x388) = 0x64;
    // the splash
    uint8_t* sl = AR(A_SPLASH);
    *(uint32_t*)sl = 0x004dd660;
    *(int32_t*)(sl + 8) = 0x65;
    for (int k = 0; k < 96; k++) ((uint16_t*)(sl + 0xcc))[k] = (uint16_t)(rnd() % 15);
    for (int k = 0; k < 15; k++) { uint8_t* v = sl + 0x18c + 32 * k; rnd_p3(v, -300.0f, 300.0f); for (int j = 4; j < 8; j++) *(uint32_t*)(v + 4 * j) = rnd(); }
    // the model object
    uint8_t* mo = AR(A_MODEL);
    *(uint32_t*)mo = 0x004dd3b0;
    random_frame((float*)(mo + 4), 300.0f);
    *(int32_t*)(mo + 0x3c) = 0x66;
    // a deleting destructor's object
    uint8_t* dd = AR(A_DDOBJ);
    *(uint32_t*)dd = 0x004dd5d8;
    *(int32_t*)(dd + 0xd0) = 0x67;
    // the track graph
    g_graf_nodes = 0;
    *(uint8_t**)X_GRAF_ROOT = chance(3) ? 0 : graf_tree(0);
    *(int32_t*)X_FACING_N = (int32_t)(rnd() % 4);
    *(int32_t*)X_UPRIGHT_N = (int32_t)(rnd() % 4);
    for (int k = 0; k < 4; k++) {
        ((uint8_t**)0x00558968)[k] = AR(A_GRAF) + 0x50 * (rnd() % (g_graf_nodes ? g_graf_nodes : 1));
        ((uint8_t**)0x00558160)[k] = AR(A_GRAF) + 0x50 * (rnd() % (g_graf_nodes ? g_graf_nodes : 1));
    }
    g_args.i = (int)(rnd() % 4);
    g_args.b = (uint8_t)(chance(30) ? 1 : 0);
    g_args.u = chance(80) ? (rnd() & 1) : rnd();
}

static void wildify() {                          // some floats (never pointers) replaced by extreme values
    const int n = 1 + rnd() % 4;
    CarObject* c = (CarObject*)AR(A_CAR);
    for (int i = 0; i < n; i++) {
        float* f = 0;
        switch (rnd() % 10) {
        case 0: f = (float*)X_CAMERA + rnd() % 12; break;
        case 1: f = (float*)&c->frame + rnd() % 12; break;
        case 2: f = (float*)(c->msg + 0x78 + 0x38 * (rnd() % 4)) + rnd() % 13; break;
        case 3: f = &c->dust[rnd() % 16].pos.x + rnd() % 7; break;
        case 4: f = (float*)&c->wheel_frame[rnd() % 4] + rnd() % 12; break;
        case 5: f = (float*)(AR(A_SHINFO) + 0x100) + rnd() % (40 * 8); break;
        case 6: f = (float*)(AR(A_WOBJ) + 4) + rnd() % 12; break;
        case 7: f = (float*)(AR(A_SPARKS) + 4) + rnd() % (32 * 7); break;
        case 8: f = &c->needle1_min + rnd() % 4; break;
        default: f = (float*)(AR(A_XVERTS)) + rnd() % (4 * 0x100); break;
        }
        setf(f, wildf());
    }
}

// ---- running a kind -------------------------------------------------------------------------------------------------
typedef void(__fastcall* ThisV_t)(void*, int);
typedef void(__fastcall* ThisP_t)(void*, int, void*);
typedef void(__fastcall* Wheel_t)(void*, int, int, uint32_t, uint8_t);
typedef void*(__fastcall* Dd_t)(void*, int, uint32_t);
typedef void(__cdecl* V_t)();
typedef void(__cdecl* VB_t)(uint8_t);
typedef void(__cdecl* VP_t)(void*);
typedef void(__cdecl* VI_t)(int);

static uint64_t call_kind(Kind k, bool rw) {
    CarObject* car = (CarObject*)AR(A_CAR);
    const Args& g = g_args;
    switch (k) {
    case K_WORLD_DRAW: rw ? WorldDraw(g.b) : ((VB_t)0x00462200)(g.b); return 0;
    case K_DRAW_MIRROR: rw ? draw_mirror() : ((V_t)0x00462390)(); return 0;
    case K_WORLD_DRAW2D: rw ? WorldDraw2D(AR(A_CANVAS)) : ((VP_t)0x00462570)(AR(A_CANVAS)); return 0;
    case K_PHYS_TRIS: rw ? draw_physics_tris() : ((V_t)0x00462880)(); return 0;
    case K_VIEW_DRAW: rw ? ViewDraw((const Frame*)X_CAMERA) : ((VP_t)0x004665a0)((void*)X_CAMERA); return 0;
    case K_VIEW_DRAW_MIRROR: rw ? ViewDrawMirror() : ((V_t)0x00466840)(); return 0;
    case K_DRAW_TREE: rw ? draw_tree(*(uint8_t**)X_GRAF_ROOT) : ((VP_t)0x0046dff0)(*(uint8_t**)X_GRAF_ROOT); return 0;
    case K_GRAF_DRAW: rw ? GrafDraw(g.i) : ((VI_t)0x0046dc70)(g.i); return 0;
    case K_CAR_DRAW: rw ? CarObject_Draw(car, 0) : ((ThisV_t)0x0046a4c0)(car, 0); return 0;
    case K_CAR_DRAW2D: rw ? CarObject_Draw2D(car, 0, AR(A_CANVAS)) : ((ThisP_t)0x0046ad80)(car, 0, AR(A_CANVAS)); return 0;
    case K_CAR_WHEEL: rw ? CarObject_DrawWheel(car, 0, g.i, 0x42c80000, g.b) : ((Wheel_t)0x0046af10)(car, 0, g.i, 0x42c80000, g.b); return 0;
    case K_CAR_XRAY: rw ? CarObject_DrawXRay(car, 0) : ((ThisV_t)0x0046b480)(car, 0); return 0;
    case K_SHADOW: rw ? ShadowObject_Draw(AR(A_SHADOW), 0) : ((ThisV_t)0x004677e0)(AR(A_SHADOW), 0); return 0;
    case K_SMOKE: rw ? SmokeObject_Draw(AR(A_SMOKE), 0) : ((ThisV_t)0x004681f0)(AR(A_SMOKE), 0); return 0;
    case K_REFL: rw ? ReflectionObject_Draw(AR(A_REFL), 0) : ((ThisV_t)0x00468450)(AR(A_REFL), 0); return 0;
    case K_SKID: rw ? SkidObject_Draw(AR(A_SKID), 0) : ((ThisV_t)0x004688d0)(AR(A_SKID), 0); return 0;
    case K_MODEL: rw ? ModelObject_Draw(AR(A_MODEL), 0) : ((ThisV_t)0x00469ac0)(AR(A_MODEL), 0); return 0;
    case K_WOBBLE: rw ? WobbleObject_Draw(AR(A_MODEL), 0) : ((ThisV_t)0x00469b20)(AR(A_MODEL), 0); return 0;
    case K_DEBRIS: rw ? DebrisObject_Draw(AR(A_DDOBJ), 0) : ((ThisV_t)0x0046eaf0)(AR(A_DDOBJ), 0); return 0;
    case K_SPARKS: rw ? SparksObject_Draw(AR(A_SPARKS), 0) : ((ThisV_t)0x0046eb80)(AR(A_SPARKS), 0); return 0;
    case K_SPLASH: rw ? SplashObject_Draw(AR(A_SPLASH), 0) : ((ThisV_t)0x0046f530)(AR(A_SPLASH), 0); return 0;
#define DD(K, FN, ADDR) case K: return rel(rw ? FN(AR(A_DDOBJ), 0, g.u) : ((Dd_t)ADDR)(AR(A_DDOBJ), 0, g.u));
    DD(K_DD_CAR, CarObject_sdd, 0x0046cdc0)
    DD(K_DD_GHOST, GhostCarObject_sdd, 0x0046ce30)
    DD(K_DD_SHADOW, ShadowObject_sdd, 0x00468eb0)
    DD(K_DD_SMOKE, SmokeObject_sdd, 0x00468ef0)
    DD(K_DD_REFL, ReflectionObject_sdd, 0x00468f10)
    DD(K_DD_SKID, SkidObject_sdd, 0x00468f70)
    DD(K_DD_MODEL, ModelObject_vdd, 0x00469b80)
    DD(K_DD_WOBBLE, WobbleObject_vdd, 0x00469ba0)
    DD(K_DD_SPLASH, SplashObject_vdd, 0x0046f890)
    DD(K_DD_DEBRIS, DebrisObject_vdd, 0x0046eb10)
    DD(K_DD_SPARKS, SparksObject_vdd, 0x0046ed60)
#undef DD
    default: return 0;
    }
}

static void footprint_of(Kind k, Footprint& fp) {
    fp.n = 0; fp.pure = false; fp.replay_only = 0;
    CarObject* car = (CarObject*)AR(A_CAR);
    const Args& g = g_args;
    switch (k) {
    case K_WORLD_DRAW: fpof_WorldDraw(fp, g.b); break;
    case K_DRAW_MIRROR: fpof_draw_mirror(fp); break;
    case K_WORLD_DRAW2D: fpof_WorldDraw2D(fp, AR(A_CANVAS)); break;
    case K_PHYS_TRIS: fpof_draw_physics_tris(fp); break;
    case K_VIEW_DRAW: fpof_ViewDraw(fp, (const Frame*)X_CAMERA); break;
    case K_VIEW_DRAW_MIRROR: fpof_ViewDrawMirror(fp); break;
    case K_DRAW_TREE: fpof_draw_tree(fp, *(uint8_t**)X_GRAF_ROOT); break;
    case K_GRAF_DRAW: fpof_GrafDraw(fp, g.i); break;
    case K_CAR_DRAW: fpof_CarObject_Draw(fp, car, 0); break;
    case K_CAR_DRAW2D: fpof_CarObject_Draw2D(fp, car, 0, AR(A_CANVAS)); break;
    case K_CAR_WHEEL: fpof_CarObject_DrawWheel(fp, car, 0, g.i, 0x42c80000, g.b); break;
    case K_CAR_XRAY: fpof_CarObject_DrawXRay(fp, car, 0); break;
    case K_SHADOW: fpof_ShadowObject_Draw(fp, AR(A_SHADOW), 0); break;
    case K_SMOKE: fpof_SmokeObject_Draw(fp, AR(A_SMOKE), 0); break;
    case K_REFL: fpof_ReflectionObject_Draw(fp, AR(A_REFL), 0); break;
    case K_SKID: fpof_SkidObject_Draw(fp, AR(A_SKID), 0); break;
    case K_MODEL: fpof_ModelObject_Draw(fp, AR(A_MODEL), 0); break;
    case K_WOBBLE: fpof_WobbleObject_Draw(fp, AR(A_MODEL), 0); break;
    case K_DEBRIS: fpof_DebrisObject_Draw(fp, AR(A_DDOBJ), 0); break;
    case K_SPARKS: fpof_SparksObject_Draw(fp, AR(A_SPARKS), 0); break;
    case K_SPLASH: fpof_SplashObject_Draw(fp, AR(A_SPLASH), 0); break;
    default: fpof_CarObject_sdd(fp, AR(A_DDOBJ), 0, g.u); break;
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
static bool g_unmask;
static int g_time0;
static int run_guarded(Kind k, bool rw, uint64_t* ret) {
    g_log.n = 0;
    g_rand = g_script.seed;
    g_time = g_time0;
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
static bool logged(const CallLog& l, uint32_t tag) { for (uint32_t i = 0; i < l.n && i < LOG_MAX; i++) if (l.w[i] == tag) return true; return false; }
static int count_tag(const CallLog& l, uint32_t tag) { int n = 0; for (uint32_t i = 0; i < l.n && i < LOG_MAX; i++) n += l.w[i] == tag; return n; }

// ---- main ----------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 100000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_wld_draw.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);         // _amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);         // _NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);         // _FF_MSGBANNER
    patch_jmp(0x004d5c10, (void*)&stub_crt_fatal);         // _fptrap
    patch_jmp(0x004d6850, (void*)&stub_crt_msgbox);        // __crtMessageBoxA
    ((void(__cdecl*)())0x004ce150)();                      // the CRT's floating point, as its start-up leaves it
    const struct { uint32_t at; void* to; } stubs[] = {
        {0x0044ecd0, (void*)&s_SetView}, {0x0044ee70, (void*)&s_SetCamera}, {0x00455ce0, (void*)&s_FinishDeferred},
        {0x0044e980, (void*)&s_EndFrame}, {0x0044e970, (void*)&s_BeginFrame}, {0x00455d00, (void*)&s_ModelDraw},
        {0x00455ec0, (void*)&s_ModelDrawI}, {0x00455c80, (void*)&s_ModelBuild}, {0x00457090, (void*)&s_SetAlpha},
        {0x004570b0, (void*)&s_ClearAlpha}, {0x00457890, (void*)&s_Enable}, {0x00457b00, (void*)&s_Disable},
        {0x00457d80, (void*)&s_IsEnabled}, {0x0044ede0, (void*)&s_SetProjection}, {0x004570d0, (void*)&s_SetClipDist},
        {0x0044f360, (void*)&s_BeginTriangles}, {0x0044f3d0, (void*)&s_DrawTriangle}, {0x0044f420, (void*)&s_EndTriangles},
        {0x0044f400, (void*)&s_DrawIndexTriangles}, {0x004562d0, (void*)&s_SetFrame}, {0x00455cc0, (void*)&s_EnterDeferred},
        {0x00455cd0, (void*)&s_LeaveDeferred}, {0x0044efb0, (void*)&s_GetCameraPos}, {0x0044f190, (void*)&s_CameraDistanceSqr},
        {0x0044f2e0, (void*)&s_SphereCanSee}, {0x00457e30, (void*)&s_SetBlendMode}, {0x00457ea0, (void*)&s_PushState},
        {0x00457ed0, (void*)&s_PopState}, {0x0045c050, (void*)&s_LightSpecial}, {0x00455cf0, (void*)&s_EnvMap},
        {0x00457050, (void*)&s_GetVerts}, {0x00457030, (void*)&s_GetInfo}, {0x0044f200, (void*)&s_ProjectPoint},
        {0x0044ee20, (void*)&s_MirrorView}, {0x00452790, (void*)&s_Line}, {0x0044fe30, (void*)&s_SetCanvas},
        {0x00453c40, (void*)&s_TextCentered}, {0x004535f0, (void*)&s_DrawStamp}, {0x00415000, (void*)&s_SingleEnter},
        {0x00415070, (void*)&s_SingleLeave}, {0x00413b40, (void*)&s_PTimeNow}, {0x004695f0, (void*)&s_TrackDraw},
        {0x00465b30, (void*)&s_TerrainGetHeight}, {0x00465b90, (void*)&s_Isect4}, {0x00465be0, (void*)&s_Isect5},
        {0x00464490, (void*)&s_CarMgrGetInfo}, {0x004a23c0, (void*)&s_MultiEnabled}, {0x0040d4c0, (void*)&s_HackShow},
        {0x004cf0a0, (void*)&s_sprintf}, {0x004112b0, (void*)&s_panic}, {0x0041b6e0, (void*)&s_Random},
        {0x00414390, (void*)&s_delete}, {0x00414300, (void*)&s_MemFree}, {0x0046bca0, (void*)&s_FindSmoke},
        {0x00467fa0, (void*)&s_Puff}, {0x0044e280, (void*)&s_ForgetTexture},
        {0x0046a170, (void*)&s_dtor}, {0x0046cd40, (void*)&s_dtor}, {0x004676c0, (void*)&s_dtor}, {0x00467f40, (void*)&s_dtor},
        {0x004682f0, (void*)&s_dtor}, {0x00468890, (void*)&s_dtor}, {0x00469aa0, (void*)&s_dtor}, {0x00469850, (void*)&s_dtor},
        {0x0046f730, (void*)&s_dtor},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    if (getenv("VP_XPRD")) patch_jmp(0x0043b120, (void*)&s_Cross);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_arena_snap = (uint8_t*)malloc(ARENA_BYTES);
    g_arena_after = (uint8_t*)malloc(ARENA_BYTES);
    g_data_snap = (uint8_t*)malloc(DATA_BYTES);
    g_data_after = (uint8_t*)malloc(DATA_BYTES);

    static Footprint fp;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    int unmasked_runs = 0, differ = 0, faults = 0, fault_both = 0, fp_bad = 0, replay_only = 0, wild_runs = 0, wild_differ = 0;
    int per[N_KINDS] = {0}, per_bad[N_KINDS] = {0};
    long long log_words = 0, checks = 0;
    // coverage
    int c_cockpit = 0, c_body = 0, c_brake = 0, c_nowheels = 0, c_wheels = 0, c_xray_car = 0, c_dust = 0, c_puff = 0,
        c_spawn = 0, c_glow = 0, c_blur = 0, c_alpha_sorted = 0, c_mirror_drawn = 0, c_facing = 0, c_upright = 0,
        c_panic = 0, c_lod_draw = 0, c_shadow_hit = 0, c_shadow_miss = 0, c_tag = 0, c_status = 0, c_sparks = 0,
        c_skid_quads = 0, c_skid_clear = 0, c_phys = 0, c_refl = 0, c_stretch = 0, c_sky = 0, c_delete = 0;
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weight((Kind)i);
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), kk = 0;
        while (pick >= kind_weight((Kind)kk)) pick -= kind_weight((Kind)kk++);
        const Kind kind = (Kind)kk;
        random_world();
        const bool wild = chance(10);
        if (wild) { wildify(); wild_runs++; }
        g_pc = chance(50) ? _PC_53 : _PC_24;
        g_unmask = chance(30);
        if (g_unmask) unmasked_runs++;
        g_time0 = (int)(rnd() % 100000);
        g_log_uv = kind != K_PHYS_TRIS && kind != K_WORLD_DRAW;
        if (trace) printf("world %d: %s\n", it, kind_names[kind]);
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
        checks++;
        log_words += g_log_orig.n;
        if (fo || fn) {
            faults++;
            if (fo && fn) { fault_both++; if (fault_both <= 8) printf("  (world %d, %s: both faulted, %08x at %08x)\n", it, kind_names[kind], fo_code, fo_eip); }
            if (fo != fn) {
                printf("  world %d (%s): %s / %s; fault %08x at %08x (original's %08x at %08x), address %08x\n", it, kind_names[kind],
                       fo ? "original faulted" : "original ran", fn ? "rewrite faulted" : "rewrite ran", g_fault_code, g_fault_eip,
                       fo_code, fo_eip, g_fault_addr);
                differ++; per_bad[kind]++;
            }
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
                    printf("    first at %u: %08x / %08x (tags before:", i, g_log_orig.w[i], g_log.w[i]);
                    for (uint32_t j = i > 40 ? i - 40 : 0; j < i; j++) { uint32_t t = g_log_orig.w[j]; if (((t >> 24) & 0xff) >= 'A' && ((t >> 24) & 0xff) <= 'Z' && ((t >> 16) & 0xff) >= 'A') printf(" %c%c%c%c@%u", t >> 24, (t >> 16) & 0xff, (t >> 8) & 0xff, t & 0xff, j); }
                    printf(")\n");
                    break;
                }
            same = false;
        }
        if (!same && getenv("VP_DUMP") && atoi(getenv("VP_DUMP")) == it) {
            const uint32_t* cp = (const uint32_t*)(g_arena_snap + A_CAMPOS);
            printf("    campos %08x %08x %08x\n", cp[0], cp[1], cp[2]);
            for (int k = 0; k < *(int32_t*)(g_data_after + (X_UPRIGHT_N - 0x4e1000)) && k < 20; k++) {
                const uint8_t* nd = ((uint8_t**)(g_data_after + (0x558160 - 0x4e1000)))[k];
                const uint32_t* q = (const uint32_t*)(g_arena_snap + (nd - g_arena) + 0x3c);
                printf("    upright %d: %08x %08x %08x\n", k, q[0], q[1], q[2]);
            }
            for (uint32_t i = 0; i < g_log_orig.n && i < LOG_MAX; i++) {
                const uint32_t a = g_log_orig.w[i], b = i < g_log.n ? g_log.w[i] : 0;
                printf("    %4u %08x %08x %s %.9g\n", i, a, b, a == b ? "  " : "<>", bitsf(a));
            }
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
        // coverage, from the original's log and state
        const CallLog& L = g_log_orig;
        CarObject* car0 = (CarObject*)(g_arena_snap + A_CAR);
        if (kind == K_CAR_DRAW) {
            if (count_tag(L, 'MDRW') >= 4 && logged(L, 'LSPC') && *(int32_t*)(g_data_snap + (X_CAMTYPE - 0x4e1000)) == 0) c_cockpit++;
            if (logged(L, 'SCLP')) c_body++;
            if (logged(L, 'PUSH')) c_brake++;
            if (logged(L, 'DTRI')) c_dust++;
            if (logged(L, 'THGT') || count_tag(L, 'GVRT')) c_xray_car++;
            (car0->lod_p ? (count_tag(L, 'FSMK') || count_tag(L, 'MDRW') > 2 ? c_wheels : c_nowheels) : c_nowheels)++;
        }
        if (kind == K_CAR_WHEEL) {
            if (logged(L, 'PUFF')) c_puff++;
            if (logged(L, 'RAND')) c_spawn++;
            if (logged(L, 'BLND')) c_glow++;
            if (logged(L, 'MDRW')) c_blur++;
        }
        if (kind == K_CAR_XRAY && logged(L, 'MBLD')) c_stretch++;
        if (kind == K_WORLD_DRAW && count_tag(L, 'GGTZ') >= 2) c_alpha_sorted++;
        if (kind == K_DRAW_MIRROR && logged(L, 'GDRW')) c_mirror_drawn++;
        if (kind == K_GRAF_DRAW || kind == K_DRAW_TREE) {
            if (logged(L, 'PANC')) c_panic++;
            if (logged(L, 'CDSQ')) c_lod_draw++;
            const int nf = *(int32_t*)(g_data_after + (X_FACING_N - 0x4e1000)), nu = *(int32_t*)(g_data_after + (X_UPRIGHT_N - 0x4e1000));
            if (nf) c_facing++;
            if (nu) c_upright++;
        }
        if (kind == K_SHADOW) (logged(L, 'MBLD') && memcmp(g_arena_snap + A_SHCOPY + 0x100, g_arena_after + A_SHCOPY + 0x100, 12) ? c_shadow_hit : c_shadow_miss)++;
        if (kind == K_CAR_DRAW2D) { if (logged(L, 'SPRF')) c_tag++; if (logged(L, 'STMP')) c_status++; }
        if (kind == K_SPARKS && logged(L, 'DTRI')) c_sparks++;
        if (kind == K_SKID) { if (logged(L, 'DITR')) c_skid_quads++; if (!logged(L, 'BTRI')) c_skid_clear++; }
        if (kind == K_PHYS_TRIS && logged(L, 'DTRI')) c_phys++;
        if (kind == K_REFL && logged(L, 'MDRW')) c_refl++;
        if (kind == K_VIEW_DRAW && logged(L, 'MDRW')) c_sky++;
        if (kind >= K_DD_CAR && logged(L, 'DELE')) c_delete++;
    }
    printf("%d worlds (%d wild, %d with overflow / divide-by-zero unmasked): %d differ (%d of them wild), %d faulted (%d in both), %d changed bytes outside the footprint, %d replay-only\n",
           iterations, wild_runs, unmasked_runs, differ, wild_differ, faults, fault_both, fp_bad, replay_only);
    printf("%lld checks, %lld logged words compared\n", checks, log_words);
    printf("per function (worlds / differing):\n");
    for (int i = 0; i < N_KINDS; i++) printf("  %-28s %6d / %d\n", kind_names[i], per[i], per_bad[i]);
    printf("CarObject::Draw: cockpit %d, body %d, brake lights %d, dust drawn %d, wheels %d / none %d, x-ray %d\n",
           c_cockpit, c_body, c_brake, c_dust, c_wheels, c_nowheels, c_xray_car);
    printf("DrawWheel: drawn %d, brake glow %d, puffs %d, dust spawned %d; DrawXRay stretched %d\n", c_blur, c_glow, c_puff, c_spawn, c_stretch);
    printf("WorldDraw sorted >= 2 %d; mirror drew objects %d; graf: LOD tests %d, facing %d, upright %d, panics %d\n",
           c_alpha_sorted, c_mirror_drawn, c_lod_draw, c_facing, c_upright, c_panic);
    printf("shadow hit %d / missed %d; reflection drawn %d; tag %d, status %d; sparks drawn %d; skid quads %d, cleared %d; physics tris %d; sky %d; deletes %d\n",
           c_shadow_hit, c_shadow_miss, c_refl, c_tag, c_status, c_sparks, c_skid_quads, c_skid_clear, c_phys, c_sky, c_delete);
    return differ || fp_bad ? 1 : 0;
}
