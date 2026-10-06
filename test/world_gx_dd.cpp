// world_gx_dd.cpp -- step G2's dd.obj (hook/gx_dd.cpp: the 63 thin DirectX wrappers, now on the OpenGL renderer) and the
// five draw wrappers of dx.obj (hook/gx_dx.cpp, which draw through the renderer too) against the originals, outside the
// game (docs/PORTING.md, checking step 3), with the REAL renderer (hook/gl_table.cpp, gl_core.cpp, ddraw_gl.cpp) and the
// real check protocol port.cpp runs in the game.
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC /I..\sdl2\SDL2-2.32.10\include
//        test\world_gx_dd.cpp hook\gl_table.cpp hook\gl_core.cpp hook\gl_dxgi.cpp hook\ddraw_gl.cpp
//        /Fo%TEMP%\g2dd\
//        /Fe%TEMP%\g2dd\world_gx_dd.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//        ..\sdl2\SDL2-2.32.10\lib\x86\SDL2.lib delayimp.lib /DELAYLOAD:SDL2.dll dxguid.lib
//   run:   world_gx_dd.exe [rounds] [seed] [isolated|chain|both]      (VP_VRMOD=1: against vrmod's race.exe, vrmod_aspect)
//                                                                  (from the repository root: out\race_v10.exe)
//   (SDL2 is delay-loaded and never called: the renderer is started headless, so SDL2.dll isn't needed.)
//   (gl_dxgi.cpp, the DXGI present, is linked but never started: start_headless leaves gl_api.SwapWindow the fake.)
//
// Loads out\race_v10.exe at 0x400000 in a child process as world_gx_dx does, resolves KERNEL32's imports into the game's
// own import slots and points its DDRAW imports at what ddraw_gl.cpp's emu_DirectDrawCreate / emu_DirectDrawEnumerateA
// do (a new gfx::DirectDraw1, gfx::enum_drivers): the ORIGINALS reach the renderer through its COM facade, exactly as in
// the game, and the rewrites call it directly.
//
// OpenGL is a set of RECORDING FAKES in gl_api (every function of gl_api.inc, plus SwapWindow / GetDrawableSize /
// SetView): Gen* hand out fresh names from a counter, CheckFramebufferStatus is complete, ReadPixels fills the page with
// random pixels from the round's stream, GetDrawableSize answers the round's window size (changed now and then at a
// flip). The renderer is started without a window (gfx::start_headless, the one test entry point in gl_core.cpp). The
// fakes count the calls that reach them by the check's phase, so the harness also checks the protocol's own claims: the
// rewrite's pass reaches no OpenGL function, and the original's pass makes every call it records exactly once.
//
// port.h's check interface is implemented here as port.cpp implements it: shadow_com_phase (1 in the original's pass,
// 2 in the rewrite's), shadow_com_effect (the pass's output log, each record [method][arguments][data, or its size
// and hash past 64 bytes]), shadow_com_check (a counter per check: gl_table.cpp keys its queue of answers to it),
// shadow_keep_original, shadow_set_state (gl_core.cpp's ShadowState). Each check:
//   * the arena (the game-side wrappers, the heap, the argument buffers; write-watched) and the image statics are kept,
//     ShadowState::begin, the ORIGINAL runs (phase 1: OpenGL calls made on the fakes, answers queued),
//     ShadowState::after_original, the arena and statics put back, the REWRITE runs (phase 2: nothing made, answers fed);
//   * compared: the return value, any fault / panic / exit, the game-side stubs' log (LogReport, MemAlloc, delete,
//     ExitProcess, the callbacks), the arena and statics, the two passes' OpenGL output logs (the first differing call
//     named by ShadowState::record_name), ShadowState::differs (the renderer's objects and state), a rewrite that did
//     something only the original may (shadow_keep_original in phase 2), and the protocol claims above;
//   * ShadowState::end(false): the original's results stand; the harness checks the renderer is then exactly as the
//     original's pass left it (the device state, the page, the texture handle list, and every object the world holds,
//     saved right after phase 1);
//   * every byte the original changed in the game's memory lies in the rewrite's footprint, unless it is replay_only.
// replay_only functions (they allocate, free or enumerate) are checked both ways as world_gx_dx checks them -- here the
// heap is the harness's and the renderer's objects go through the check machinery -- without the footprint rule.
// "isolated": the rewrite alone (its callees the originals); "chain": every rewrite of the group patched into the image
// for the rewrite's pass.
//
// The worlds: a DirectDraw2 (dd_create), the display mode, the primary with its back buffer, a z-buffer, Direct3D and
// the device (d3d::d3d, connect), textures of random formats (565, 555, 1555, 4444), power-of-two sizes 1..256, mip
// counts and colour keys, offscreen images, materials and viewports -- all made by the ORIGINALS, unchecked, as the game
// makes them -- and the game-side wrappers (ddraw 4 bytes, dsurface 4, dtexture 0x10, d3d 8, dmaterial 8, dviewport 4)
// in the arena. Then each of the 68 functions with random arguments: locks and unlocks of textures and of the back
// buffer (the "game's 2D" writes random pixels between them), blits, colour keys, texture loads and selects, mip walks,
// viewports (w = 0 now and then), clears, transforms, render states, transforms, caps, flips, and draws of random
// LVERTEX / TLVERTEX triangles with a bound texture half the time -- so the texture upload runs -- and different garbage
// in the fields Direct3D never reads in each pass, so the check's vertex sanitising is what makes them match.
// Destructors get objects made for them alone. Objects the world holds are freed at each round's end.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define SDL_MAIN_HANDLED                 // (SDL.h, through gl_table.h, would rename main)
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <type_traits>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif
#define VP_FAITHFUL
#define VP_PORT_NEEDS_RENDERER(NEW)      // PORT_FN_GL: no PortFn to mark here (the registry below replaces PORT_FN)
#include "../hook/port.h"
#include "../hook/gx_gl.h"
#include "../hook/gl_dxgi.h"

// ---- the registry: every PORT_FN, with a uniform caller (every argument is one stack dword) ----------------------------
struct Reg {
    uint32_t at;
    const char* name;
    void* fn;
    uint64_t (*run)(int rewrite, uint32_t at, const uint32_t* a, Footprint* f);
    int nargs;
    Reg* next;
    static Reg*& head() { static Reg* h; return h; }
    Reg(uint32_t t, const char* n, void* f, uint64_t (*r)(int, uint32_t, const uint32_t*, Footprint*), int na)
        : at(t), name(n), fn(f), run(r), nargs(na), next(head()) { head() = this; }
};
template <typename T> static T cv(uint32_t v) {
    if constexpr (std::is_pointer_v<T>) return (T)(uintptr_t)v;
    else return (T)v;
}
template <typename F> struct Inv;
#if defined(__GNUC__) && !defined(__clang__)
// GCC drops a calling convention from the type of a non-type template parameter (`Fn NEW` would become a __cdecl
// pointer that a __stdcall/__fastcall rewrite doesn't convert to): take NEW as `auto` (it keeps its own type, = Fn)
#define INV_CC(CC)                                                                                                     \
    template <typename R, typename... Args> struct Inv<R(CC*)(Args...)> {                                             \
        typedef R(CC* Fn)(Args...);                                                                                    \
        enum { N = sizeof...(Args) };                                                                                  \
        template <auto NEW, void (*FP)(Footprint&, Args...), size_t... I>                                              \
        static uint64_t go(int rw, uint32_t at, const uint32_t* a, Footprint* f, std::index_sequence<I...>) {          \
            if (f) { FP(*f, cv<Args>(a[I])...); return 0; }                                                            \
            Fn fn = rw ? NEW : (Fn)(uintptr_t)at;                                                                      \
            if constexpr (std::is_void_v<R>) { fn(cv<Args>(a[I])...); return 0; }                                     \
            else { R r = fn(cv<Args>(a[I])...); uint64_t u = 0; memcpy(&u, &r, sizeof r); return u; }                  \
        }                                                                                                              \
        template <auto NEW, void (*FP)(Footprint&, Args...)>                                                           \
        static uint64_t run(int rw, uint32_t at, const uint32_t* a, Footprint* f) {                                    \
            return go<NEW, FP>(rw, at, a, f, std::index_sequence_for<Args...>{});                                      \
        }                                                                                                              \
    };
#else
#define INV_CC(CC)                                                                                                     \
    template <typename R, typename... Args> struct Inv<R(CC*)(Args...)> {                                             \
        typedef R(CC* Fn)(Args...);                                                                                    \
        enum { N = sizeof...(Args) };                                                                                  \
        template <Fn NEW, void (*FP)(Footprint&, Args...), size_t... I>                                                \
        static uint64_t go(int rw, uint32_t at, const uint32_t* a, Footprint* f, std::index_sequence<I...>) {          \
            if (f) { FP(*f, cv<Args>(a[I])...); return 0; }                                                            \
            Fn fn = rw ? NEW : (Fn)(uintptr_t)at;                                                                      \
            if constexpr (std::is_void_v<R>) { fn(cv<Args>(a[I])...); return 0; }                                     \
            else { R r = fn(cv<Args>(a[I])...); uint64_t u = 0; memcpy(&u, &r, sizeof r); return u; }                  \
        }                                                                                                              \
        template <Fn NEW, void (*FP)(Footprint&, Args...)>                                                             \
        static uint64_t run(int rw, uint32_t at, const uint32_t* a, Footprint* f) {                                    \
            return go<NEW, FP>(rw, at, a, f, std::index_sequence_for<Args...>{});                                      \
        }                                                                                                              \
    };
#endif
INV_CC(__cdecl)
INV_CC(__fastcall)
INV_CC(__stdcall)
#undef INV_CC
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                               \
    static Reg VP_CAT(reg_, NEW)(V10, NAME, (void*)&NEW, &Inv<decltype(&NEW)>::run<&NEW, &FP>, Inv<decltype(&NEW)>::N);

void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 4, what); }

// gx_dx.cpp for its five draw wrappers, in a namespace of its own (its constants would clash with gx_dd.cpp's), and
// first: its HRESULT names would be taken by ddraw.h's macros, which gx_dd.cpp brings in (gl_core.h)
namespace gxdx {
#include "../hook/gx_dx.cpp"
}
#include "../hook/gx_dd.cpp"
#include "vrmod_image.h"
static bool g_vrmod;                    // VP_VRMOD=1 (vrmod_aspect, below)

// ---- what the renderer needs from the DLL (platform.cpp, viperport.cpp, port.cpp) --------------------------------------
void logf(const char*, ...) {}                                  // (the renderer's log lines: not compared)
SDL_Window* platform_window() { return (SDL_Window*)(uintptr_t)0x5d100; }   // never dereferenced: the fakes take it
void platform_set_view(int, int, int, int, int, int) {}
// the session recorder (session.cpp), off: what gl_core.cpp reads of it
volatile int g_session_mode = 0;                                 // SESSION_OFF
bool g_session_frames = false;
void session_gfx(uint8_t, const void*, size_t, const void*, size_t, const void*, size_t) {}
void session_gfx_state(uint32_t, const void*, size_t) {}
void session_gfx_page(const uint16_t*, int, int, const uint8_t*) {}
void session_frame() {}
uint64_t session_hash(const void*, size_t, uint64_t h) { return h; }
bool session_render_size(int*, int*) { return false; }           // no replay: the target follows the window

// port.h's check interface, as port.cpp has it (see the header)
static int g_com_phase;                                         // 0 outside a check's passes, 1 original, 2 rewrite
static unsigned g_com_checks;
static std::vector<std::string> g_glout[2];                     // each pass's output log, one record per call
static const ShadowState* g_state;
static const char* g_keep[2];                                   // shadow_keep_original, by pass
int shadow_com_phase() { return g_com_phase; }
unsigned shadow_com_check() { return g_com_checks; }
void shadow_com_effect(uint16_t method, const void* self, const void* args, size_t nargs, const void* data, size_t ndata) {
    if (!g_com_phase) return;
    uint8_t rec[8 + 64 + 12];                                   // method, the arguments, then the data or its hash
    size_t n = 0;
    memcpy(rec, &method, 2); n = 2;
    if (nargs > 64) nargs = 64;
    memcpy(rec + n, args, nargs); n += nargs;
    std::string r((const char*)&self, 4);                       // (port.cpp's record carries self; the renderer's is 0)
    if (data && ndata <= 64) {
        r.append((const char*)rec, n);
        r.append((const char*)data, ndata);
    } else {
        uint64_t h = 1469598103934665603ull;                    // FNV-1a over the data
        for (size_t i = 0; data && i < ndata; i++) h = (h ^ ((const uint8_t*)data)[i]) * 1099511628211ull;
        uint32_t nd = (uint32_t)ndata;
        memcpy(rec + n, &nd, 4); n += 4;
        memcpy(rec + n, &h, 8); n += 8;
        r.append((const char*)rec, n);
    }
    g_glout[g_com_phase - 1].push_back(r);
}
void shadow_set_state(const ShadowState* s) { g_state = s; }
void shadow_keep_original(const char* what) {
    if (g_com_phase && !g_keep[g_com_phase - 1]) g_keep[g_com_phase - 1] = what;
}

// ---- the fake OpenGL ------------------------------------------------------------------------------------------------------
namespace gpu {
enum Id {
#define GL_API_FN(ret, name, args) G_##name,
#include "../hook/gl_api.inc"
#undef GL_API_FN
    G_SwapWindow, G_GetDrawableSize, G_SetView, G_N
};
static long calls[3];                   // the calls that reached the "GPU", by the check phase they came in
static long by_id[G_N];
static uint32_t names = 1;              // the next name a Gen hands out
static uint32_t rng = 1;                // the round's stream (ReadPixels)
static int pack = 4;                    // GL_PACK_ALIGNMENT as the GPU has it
static int dw = 1920, dh = 1080;        // the window's drawable size
static uint32_t next() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static void note(int id) {
    calls[g_com_phase]++;
    by_id[id]++;
}
template <int ID, typename F> struct Fake;
template <int ID, typename R, typename... A> struct Fake<ID, R(APIENTRY*)(A...)> {
    static R APIENTRY f(A...) {
        note(ID);
        return R();
    }
};
template <int ID> static void APIENTRY gen(GLsizei n, GLuint* out) {
    note(ID);
    for (GLsizei i = 0; i < n; i++) out[i] = names++;
}
static GLenum APIENTRY check_fb(GLenum) {
    note(G_CheckFramebufferStatus);
    return 0x8CD5;                                              // GL_FRAMEBUFFER_COMPLETE
}
static void APIENTRY pixel_store(GLenum a, GLint v) {
    note(G_PixelStorei);
    if (a == 0x0D05) pack = v;                                  // GL_PACK_ALIGNMENT
}
static void APIENTRY read_pixels(GLint, GLint, GLsizei w, GLsizei h, GLenum f, GLenum t, void* out) {
    note(G_ReadPixels);
    if (w <= 0 || h <= 0) return;
    size_t bpp = t == GL_UNSIGNED_BYTE ? (f == GL_RGB || f == 0x80E0 /*GL_BGR*/ ? 3 : 4) : 2;
    size_t row = ((size_t)w * bpp + pack - 1) / pack * pack;
    for (size_t i = 0; i < row * h; i++) ((uint8_t*)out)[i] = (uint8_t)next();
}
static const GLubyte* APIENTRY get_string(GLenum) {
    note(G_GetString);
    return (const GLubyte*)"world_gx_dd's fake OpenGL";
}
static GLuint APIENTRY create_name() {
    note(G_CreateProgram);
    return names++;
}
static GLuint APIENTRY create_shader(GLenum) {
    note(G_CreateShader);
    return names++;
}
static void APIENTRY get_iv(GLuint, GLenum, GLint* v) {
    note(G_GetShaderiv);
    *v = 1;
}
static void swap_window(SDL_Window*) { note(G_SwapWindow); }
static void drawable_size(SDL_Window*, int* w, int* h) {
    note(G_GetDrawableSize);
    *w = dw, *h = dh;
}
static void set_view(int, int, int, int, int, int) { note(G_SetView); }

static void install() {
#define GL_API_FN(ret, name, args) gl_api.name = &Fake<G_##name, decltype(gl_api.name)>::f;
#include "../hook/gl_api.inc"
#undef GL_API_FN
    gl_api.GenTextures = &gen<G_GenTextures>;
    gl_api.GenFramebuffers = &gen<G_GenFramebuffers>;
    gl_api.GenRenderbuffers = &gen<G_GenRenderbuffers>;
    gl_api.GenBuffers = &gen<G_GenBuffers>;
    gl_api.GenVertexArrays = &gen<G_GenVertexArrays>;
    gl_api.CheckFramebufferStatus = &check_fb;
    gl_api.PixelStorei = &pixel_store;
    gl_api.ReadPixels = &read_pixels;
    gl_api.GetString = &get_string;
    gl_api.CreateProgram = &create_name;
    gl_api.CreateShader = &create_shader;
    gl_api.GetShaderiv = &get_iv;
    gl_api.GetProgramiv = &get_iv;
    gl_api.SwapWindow = &swap_window;
    gl_api.GetDrawableSize = &drawable_size;
    gl_api.SetView = &set_view;
}
}  // namespace gpu

namespace hx {
static void vrmod_aspect();                // VP_VRMOD (below)
// ---- random values ------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static uint32_t fnv(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}

// ---- the original, loaded at 0x400000 -------------------------------------------------------------------------------
struct IatSlot { std::string dll, name; uint32_t at; };
static std::vector<IatSlot> g_iat;
static uint32_t iat_slot(const char* name) {
    for (auto& s : g_iat)
        if (s.name == name) return s.at;
    return 0;
}
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
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.Size && d->Name; d++) {
        const char* dll = (const char*)(base + d->Name);
        bool k32 = !_stricmp(dll, "KERNEL32.dll");
        HMODULE m = GetModuleHandleA("kernel32.dll");
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            g_iat.push_back({dll, fn, (uint32_t)(uintptr_t)&iat->u1.Function});
            if (!k32) continue;
            FARPROC p = GetProcAddress(m, fn);
            if (p) iat->u1.Function = (DWORD)(uintptr_t)p;
        }
    }
    free(file);
    return true;
}
static int relaunch() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
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
    if (code > 4) printf("the test process ended with 0x%08lx\n", code);
    return code > 4 ? 5 : (int)code;
}
static void patch_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// ---- the arena: write-watched; the shadow holds its content as of the last check ---------------------------------
enum : uint32_t {
    PG = 4096, ARENA_SIZE = 24u << 20, NPAGES = ARENA_SIZE / PG,
    GAME_OFF = 0x10000,                                    // the game's memory from here (below: the stubs' state)
    KEEP_OFF = 0x10000, KEEP_SIZE = 0x40000,               // the round's wrappers (never reused within a round)
    WORLD_OFF = 0x50000, WORLD_SIZE = 0x200000,            // argument buffers (reused, wrapping)
    HEAP_OFF = 0x250000,                                   // MemAlloc
};
static uint8_t* g_ar;
static uint8_t* g_sh;
static ULONG_PTR* g_ww;
static std::vector<uint32_t> ww_take() {
    ULONG_PTR n = NPAGES;
    DWORD gran = 0;
    std::vector<uint32_t> v;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_ar, ARENA_SIZE, (PVOID*)g_ww, &n, &gran)) {
        printf("GetWriteWatch failed (%lu)\n", GetLastError());
        ExitProcess(4);
    }
    v.reserve(n);
    for (ULONG_PTR i = 0; i < n; i++) v.push_back((uint32_t)((g_ww[i] - (ULONG_PTR)g_ar) / PG));
    std::sort(v.begin(), v.end());
    return v;
}
static void ww_sync() {
    for (uint32_t p : ww_take()) memcpy(g_sh + (size_t)p * PG, g_ar + (size_t)p * PG, PG);
}
static bool in_arena(const void* p, size_t n = 1) {
    return (uintptr_t)p >= (uintptr_t)g_ar && (uintptr_t)p + n <= (uintptr_t)g_ar + ARENA_SIZE;
}
static uint32_t aoff(const void* p) {                       // a pointer for the log: arena offset, image address, or "other"
    uintptr_t a = (uintptr_t)p;
    if (!a) return 0;
    if (in_arena(p)) return 0xa0000000u | (uint32_t)(a - (uintptr_t)g_ar);
    if (a >= 0x400000 && a < 0x700000) return (uint32_t)a;
    return 0x5ac00000u;                                    // the stack or the renderer's heap (differs by pass or not)
}

// the stubs' state, in the arena (saved, restored and compared with everything else)
struct Ctrl {
    uint32_t fr;                 // the stubs' random stream (callbacks' answers, MemAlloc's nulls)
    uint32_t heap_top;           // MemAlloc's next offset
    int32_t oom_pct;             // MemAlloc nulls (%)
};
static Ctrl* C() { return (Ctrl*)g_ar; }
static uint32_t frnd() {
    uint32_t x = C()->fr;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    C()->fr = x;
    return x;
}

// ---- the log of the game-side stubs ---------------------------------------------------------------------------------------
static std::vector<uint32_t> g_log[2];
static int g_pass;
enum { LOG_CAP = 1 << 20 };
static void lg(uint32_t v) {
    if (g_log[g_pass].size() < LOG_CAP) g_log[g_pass].push_back(v);
}
#if defined(__GNUC__) && !defined(__clang__)
static uint32_t safe_hash(const void* p, uint32_t n) {
    if (!p) return 0xdeadbeef;
    uint32_t r = 0;
    if (vp_try([&] { r = fnv(p, n); })) return 0xbadbad;
    return r;
}
static uint32_t safe_hash_str(const char* s, int max = 512) {
    if (!s) return 0xdeadbeef;
    uint32_t h = 2166136261u;
    if (vp_try([&] {
            volatile uint32_t& vh = h;                   // its value at a fault stays in memory
            for (int i = 0; i < max && s[i]; i++) vh = (vh ^ (uint8_t)s[i]) * 16777619u;
        })) { h ^= 0xbadbad; }
    return h;
}
static void lg_fmt(const char* fmt, va_list ap) {
    lg(aoff(fmt));
    lg(safe_hash_str(fmt));
    if (vp_try([&] {
            for (const char* p = fmt; *p; p++) {
                if (*p != '%') continue;
                p++;
                while (*p && strchr("-+ #0123456789.lh", *p)) p++;
                if (!*p) break;
                if (*p == '%') continue;
                if (*p == 's') lg(safe_hash_str(va_arg(ap, const char*)));
                else if (*p == 'f' || *p == 'g' || *p == 'e') {
                    double d = va_arg(ap, double);
                    uint32_t u[2];
                    memcpy(u, &d, 8);
                    lg(u[0]);
                    lg(u[1]);
                } else lg(va_arg(ap, uint32_t));
            }
        })) { lg(0xbadf0); }
}
#else
static uint32_t safe_hash(const void* p, uint32_t n) {
    if (!p) return 0xdeadbeef;
    __try {
        return fnv(p, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0xbadbad; }
}
static uint32_t safe_hash_str(const char* s, int max = 512) {
    if (!s) return 0xdeadbeef;
    uint32_t h = 2166136261u;
    __try {
        for (int i = 0; i < max && s[i]; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    } __except (EXCEPTION_EXECUTE_HANDLER) { h ^= 0xbadbad; }
    return h;
}
static void lg_fmt(const char* fmt, va_list ap) {
    lg(aoff(fmt));
    lg(safe_hash_str(fmt));
    __try {
        for (const char* p = fmt; *p; p++) {
            if (*p != '%') continue;
            p++;
            while (*p && strchr("-+ #0123456789.lh", *p)) p++;
            if (!*p) break;
            if (*p == '%') continue;
            if (*p == 's') lg(safe_hash_str(va_arg(ap, const char*)));
            else if (*p == 'f' || *p == 'g' || *p == 'e') {
                double d = va_arg(ap, double);
                uint32_t u[2];
                memcpy(u, &d, 8);
                lg(u[0]);
                lg(u[1]);
            } else lg(va_arg(ap, uint32_t));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { lg(0xbadf0); }
}
#endif
enum : uint32_t {
    L_REPORT = 0x4b000001, L_PANIC, L_ALLOC, L_FREE, L_WINDOW, L_EXIT, L_DDENUM_CB, L_MODES_CB, L_TEXFMT_CB,
};
static const DWORD PANIC_CODE = 0xE0564B01, EXIT_CODE = 0xE0564B03;

// ---- the imports: what ddraw_gl.cpp's emu_DirectDrawCreate / emu_DirectDrawEnumerateA do -------------------------------------
static HRESULT WINAPI st_dd_create(GUID*, LPDIRECTDRAW* out, IUnknown*) {
    *out = gfx::make<gfx::DirectDraw1>();
    return DD_OK;
}
static HRESULT WINAPI st_dd_enum(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    gfx::enum_drivers(cb, ctx);
    return DD_OK;
}
static void __stdcall st_exit_process(uint32_t code) {
    lg(L_EXIT);
    lg(code);
    RaiseException(EXIT_CODE, 0, 0, 0);
}

// ---- the game functions around the group ----------------------------------------------------------------------------------
static void __cdecl st_log_report(const char* fmt, ...) {
    lg(L_REPORT);
    va_list ap;
    va_start(ap, fmt);
    lg_fmt(fmt, ap);
    va_end(ap);
}
static void __cdecl st_log_panic(const char* fmt, ...) {
    lg(L_PANIC);
    va_list ap;
    va_start(ap, fmt);
    lg_fmt(fmt, ap);
    va_end(ap);
    RaiseException(PANIC_CODE, 0, 0, 0);
}
static void* __cdecl st_mem_alloc(int n) {
    lg(L_ALLOC);
    lg((uint32_t)n);
    if ((int32_t)(frnd() % 100) < C()->oom_pct) return 0;
    uint32_t sz = ((uint32_t)n + 15) & ~15u;
    if (C()->heap_top + sz + 16 > ARENA_SIZE) C()->heap_top = HEAP_OFF;
    uint8_t* p = g_ar + C()->heap_top;
    memset(p, 0xa3, sz);
    C()->heap_top += sz + 16;
    return p;
}
static void __cdecl st_delete(void* p) {
    lg(L_FREE);
    lg(aoff(p));
}
static void* __cdecl st_get_window() {
    lg(L_WINDOW);
    return (void*)(uintptr_t)0x00abc123;
}
// the harness's own callbacks (for the enumerators); their answers come from the arena's stream, the same in both passes
static int32_t __stdcall h_texfmt_cb(void* desc, void* ctx) {
    lg(L_TEXFMT_CB);
    lg(safe_hash(desc, 0x6c));
    lg(aoff(ctx));
    return frnd() % 5 != 0;
}
static int32_t __stdcall h_modes_cb(void* desc, void* ctx) {
    lg(L_MODES_CB);
    lg(safe_hash(desc, 0x6c));
    lg(aoff(ctx));
    return frnd() % 5 != 0;
}
static int32_t __stdcall h_ddenum_cb(void* guid, char* d, char* n, void* ctx) {
    lg(L_DDENUM_CB);
    lg(guid ? safe_hash(guid, 16) : 0);
    lg(safe_hash_str(d));
    lg(safe_hash_str(n));
    lg(aoff(ctx));
    return frnd() % 3 != 0;
}
static void install_stubs() {
    patch_jmp(0x00411150, (void*)&st_log_report);
    patch_jmp(0x004112b0, (void*)&st_log_panic);
    patch_jmp(0x004140e0, (void*)&st_mem_alloc);
    patch_jmp(0x00414390, (void*)&st_delete);
    patch_jmp(0x00412be0, (void*)&st_get_window);
    *(uint32_t*)0x005d7478 = (uint32_t)(uintptr_t)&st_exit_process;   // imp__ExitProcess@4
    uint32_t a = iat_slot("DirectDrawCreate"), b = iat_slot("DirectDrawEnumerateA");
    if (!a || !b) { printf("the DDRAW import slots weren't found\n"); ExitProcess(2); }
    *(uint32_t*)(uintptr_t)a = (uint32_t)(uintptr_t)&st_dd_create;
    *(uint32_t*)(uintptr_t)b = (uint32_t)(uintptr_t)&st_dd_enum;
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}

// ---- the group, and the chain: every rewrite of it patched in (for a rewrite's pass) --------------------------------------
static bool in_group(uint32_t at) {
    return (at >= 0x0045e940 && at <= 0x0045f9a0) || at == 0x00458a60 || at == 0x00458b70 || at == 0x00458b90 ||
           at == 0x00458bb0 || at == 0x00458be0;
}
struct ChainSave { uint32_t at; uint8_t b[5]; };
static std::vector<ChainSave> g_chain_save;
static void chain_on() {
    g_chain_save.clear();
    for (Reg* r = Reg::head(); r; r = r->next) {
        if (!in_group(r->at)) continue;
        ChainSave s;
        s.at = r->at;
        memcpy(s.b, (void*)(uintptr_t)r->at, 5);
        g_chain_save.push_back(s);
        patch_jmp(r->at, r->fn);
    }
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}
static void chain_off() {
    for (size_t i = g_chain_save.size(); i-- > 0;) memcpy((void*)(uintptr_t)g_chain_save[i].at, g_chain_save[i].b, 5);
    g_chain_save.clear();
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}

// ---- the image statics a check saves and compares -------------------------------------------------------------------
struct GRange { uint32_t at, n; const char* what; };
static const GRange g_statics[] = {
    {0x004f1e88, 0x50, "vid statics"},        {0x004f2314, 0x1c, "dx statics"},    {0x004f3a60, 4, "the device"},
    {0x005229d8, 0x18, "dxState current"},    {0x00522a18, 0x10, "mrCaps"},        {0x00522ac8, 0x5c, "vid screen statics"},
    {0x00522e60, 0x10, "dx context / d3d / dx_result"}, {0x00522e78, 0x94, "dx error buffer"},
    {0x00522f0c, 0x84, "dx transform data"},  {0x005265f4, 4, "dxState pointer"}, {0x00526608, 0x1c, "dxState cache / force"},
};
enum : uint32_t {
    H_V_HAS3DHW = 0x4f1e9c,     // byte (VidHas3DHW)
    H_X_D3D = 0x522e64,         // d3d* (the draw wrappers' device)
    H_X_RESULT = 0x522e6c,      // dx_result
    H_D_DEVICE = 0x4f3a60,      // IDirect3DDevice2* (dd.obj's)
};
static uint32_t& M32(uint32_t a) { return *(uint32_t*)(uintptr_t)a; }
static uint32_t statics_bytes() {
    uint32_t t = 0;
    for (auto& g : g_statics) t += g.n;
    return t;
}
static void snap_statics(std::vector<uint8_t>& v) {
    v.resize(statics_bytes());
    uint8_t* p = v.data();
    for (auto& g : g_statics) { memcpy(p, (void*)(uintptr_t)g.at, g.n); p += g.n; }
}
static void load_statics(const std::vector<uint8_t>& v) {
    const uint8_t* p = v.data();
    for (auto& g : g_statics) { memcpy((void*)(uintptr_t)g.at, p, g.n); p += g.n; }
}
static const char* static_name(uint32_t a) {
    for (auto& g : g_statics)
        if (a >= g.at && a < g.at + g.n) return g.what;
    return "?";
}

// ---- the renderer's objects the world holds (freed at the round's end; saved for the protocol check) -------------------
static std::vector<gfx::Obj*> g_objs;
static std::set<gfx::Obj*> g_obj_set;
static void hold_obj(gfx::Obj* o) {
    if (o && g_obj_set.insert(o).second) g_objs.push_back(o);
}
static gfx::Surface* as_surface(gfx::Obj* o) { return o->kind == gfx::K_SURFACE ? static_cast<gfx::Surface*>(o) : 0; }
// every object the world holds, with each surface's mip levels and back buffer
static void world_objects(std::vector<gfx::Obj*>& out) {
    out.clear();
    std::set<gfx::Obj*> seen;
    auto add = [&](gfx::Obj* o) { if (o && seen.insert(o).second) out.push_back(o); };
    for (gfx::Obj* o : g_objs) {
        add(o);
        if (gfx::Surface* s = as_surface(o)) {
            for (gfx::Surface* l = s->next_mip; l; l = l->next_mip) add(l);
            if (s->back) add(s->back);
        }
    }
}
static void free_world_objects() {
    std::vector<gfx::Obj*> all;
    world_objects(all);
    std::set<gfx::Obj*> owned;                              // levels go with their root, a back buffer with its primary
    for (gfx::Obj* o : all)
        if (gfx::Surface* s = as_surface(o)) {
            if (s->root && s->root != s) owned.insert(o);
            if (s->back) owned.insert(s->back), s->back->refs = 1;
        }
    for (gfx::Obj* o : all)
        if (!owned.count(o)) delete o;
    g_objs.clear();
    g_obj_set.clear();
}

// the renderer as a check's protocol leaves it: its state, the page, the handle list, and each object (saved)
static void put_bytes(std::vector<uint8_t>& o, const void* p, size_t n) { o.insert(o.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
struct RSnap {
    std::vector<uint8_t> st, page, under, handles;
    std::vector<gfx::Obj*> objs;
    std::vector<std::vector<uint8_t>> obj;
};
static void snap_renderer(RSnap& r, bool pick) {
    r.st.clear();
    put_bytes(r.st, &gfx::st, sizeof gfx::st);
    r.page.assign((const uint8_t*)gfx::pg.page.data(), (const uint8_t*)(gfx::pg.page.data() + gfx::pg.page.size()));
    r.under.assign((const uint8_t*)gfx::pg.under.data(), (const uint8_t*)(gfx::pg.under.data() + gfx::pg.under.size()));
    std::vector<gfx::Surface*> hs(gfx::handles.begin(), gfx::handles.end());
    r.handles.clear();
    put_bytes(r.handles, hs.data(), hs.size() * sizeof(void*));
    if (pick) {                                             // (only the living: one released for good goes at end())
        std::vector<gfx::Obj*> all;
        world_objects(all);
        r.objs.clear();
        for (gfx::Obj* o : all)
            if (o->refs > 0) r.objs.push_back(o);
    }
    r.obj.resize(r.objs.size());
    for (size_t i = 0; i < r.objs.size(); i++) r.objs[i]->save(r.obj[i]);
}
static const char* kind_name(gfx::Kind k) {
    static const char* const n[] = {"state", "page", "handles", "IDirectDraw", "IDirectDraw2", "IDirect3D2", "device",
                                    "viewport", "material", "surface"};
    return k <= gfx::K_SURFACE ? n[k] : "?";
}
static std::string rsnap_diff(const RSnap& a, const RSnap& b) {
    auto first = [](const std::vector<uint8_t>& x, const std::vector<uint8_t>& y) {
        size_t k = 0;
        while (k < x.size() && k < y.size() && x[k] == y[k]) k++;
        return k;
    };
    char buf[200];
    if (a.st != b.st) { sprintf(buf, "the device state (gfx::st) byte %zu", first(a.st, b.st)); return buf; }
    if (a.page != b.page) { sprintf(buf, "the page byte %zu (of %zu / %zu)", first(a.page, b.page), a.page.size(), b.page.size()); return buf; }
    if (a.under != b.under) { sprintf(buf, "the page's under-copy byte %zu", first(a.under, b.under)); return buf; }
    if (a.handles != b.handles) return "the texture handle list";
    for (size_t i = 0; i < a.obj.size(); i++)
        if (a.obj[i] != b.obj[i]) {
            sprintf(buf, "%s %p, saved byte %zu", kind_name(a.objs[i]->kind), (void*)a.objs[i], first(a.obj[i], b.obj[i]));
            return buf;
        }
    return "";
}

// ---- running one call both ways ---------------------------------------------------------------------------------------
static DWORD g_fault_code;
static uint32_t g_fault_at;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_at = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
#if defined(__GNUC__) && !defined(__clang__)
static int guarded(Reg* r, int rw, const uint32_t* a, uint64_t* ret) {
    if (vp_try([&] { *ret = r->run(rw, r->at, a, 0); }, fault_filter)) {
        _fpreset();
        if (g_fault_code == PANIC_CODE) return 1;
        if (g_fault_code == EXIT_CODE) return 3;
        return 2;
    }
    return 0;
}
static int guarded_fp(Reg* r, const uint32_t* a, Footprint* f) {
    if (vp_try([&] { r->run(0, r->at, a, f); })) { return 1; }
    return 0;
}
#else
static int guarded(Reg* r, int rw, const uint32_t* a, uint64_t* ret) {
    __try {
        *ret = r->run(rw, r->at, a, 0);
        return 0;
    } __except (fault_filter(GetExceptionInformation())) {
        _fpreset();
        if (g_fault_code == PANIC_CODE) return 1;
        if (g_fault_code == EXIT_CODE) return 3;
        return 2;
    }
}
static int guarded_fp(Reg* r, const uint32_t* a, Footprint* f) {
    __try {
        r->run(0, r->at, a, f);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}
#endif

struct Stat { std::string name; long n, bad, faults, panics, exits, ronly, fpbad, fpfault, fault_state, kept, proto; };
static std::vector<Stat> g_stats;
static Stat& stat(const std::string& nm) {
    for (Stat& s : g_stats)
        if (s.name == nm) return s;
    g_stats.push_back({nm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    return g_stats.back();
}
static Footprint g_fp;
static std::vector<uint8_t> g_s0, g_s1, g_after;
static RSnap g_r1, g_r2;
static bool g_chain;
static long g_mismatch_total, g_checks_total, g_fp_total, g_proto_total, g_kept_total;
static uint64_t g_gl_calls;
static int g_pc;
static const char* g_phase = "setup";
static std::string g_phase_name;
typedef void (*PrePass)(int pass);                            // before each pass (the draws' don't-care garbage)

static int run_pass(Reg* r, int rw, const uint32_t* a, uint64_t* ret) {
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, g_pc == 24 ? _PC_24 : _PC_53, _MCW_PC);
    int k = guarded(r, rw, a, ret);
    _controlfp_s(&cw, _CW_DEFAULT, 0xffffffff);
    _clearfp();
    return k;
}
static bool in_footprint(const void* p) {
    for (int k = 0; k < g_fp.n; k++)
        if ((const uint8_t*)p >= (const uint8_t*)g_fp.r[k].p && (const uint8_t*)p < (const uint8_t*)g_fp.r[k].p + g_fp.r[k].n) return true;
    return false;
}
static const char* arena_region(uint32_t off) {
    if (off < GAME_OFF) return "stub state";
    if (off < WORLD_OFF) return "wrappers";
    if (off < HEAP_OFF) return "world";
    return "heap";
}
static std::string gl_rec_name(const std::string& rec) {
    uint16_t m = 0;
    if (rec.size() >= 6) memcpy(&m, rec.data() + 4, 2);
    const char* n = g_state && g_state->record_name ? g_state->record_name(m) : 0;
    char b[64];
    if (!n) sprintf(b, "record %04x", m), n = b;
    return n;
}
static uint64_t check(Reg* r, const uint32_t* a, PrePass pre = 0) {
    std::string name = r->name;
    if (g_chain) name += " [chain]";
    g_phase_name = name;
    Stat& st = stat(name);
    st.n++;
    g_checks_total++;
    g_pc = chance(75) ? 24 : 53;                         // the main thread runs single precision after gxBegin
    ww_sync();
    snap_statics(g_s0);
    g_fp.n = 0;
    g_fp.replay_only = 0;
    g_fp.pure = false;
    int fpfault = guarded_fp(r, a, &g_fp);
    if (fpfault) st.fpfault++;
    if (g_fp.replay_only) st.ronly++;
    ww_sync();                                           // (a footprint writes nothing; to be sure)
    // the check starts (port.cpp: shadow_snapshot)
    g_com_checks++;
    g_glout[0].clear();
    g_glout[1].clear();
    g_keep[0] = g_keep[1] = 0;
    for (long& c : gpu::calls) c = 0;
    g_state->begin();
    // the original
    if (pre) pre(0);
    g_pass = 0;
    g_log[0].clear();
    uint64_t r0 = 0, r1 = 0;
    g_com_phase = 1;
    int k0 = run_pass(r, 0, a, &r0);
    g_com_phase = 0;
    DWORD c0 = k0 ? g_fault_code : 0;
    uint32_t a0 = g_fault_at;
    long gpu1 = gpu::calls[1];
    std::vector<uint32_t> d1 = ww_take();
    if (g_keep[0]) {                                     // (port.cpp: shadow_after_original, the heap case)
        g_state->end(true);                              // the original's result stands, unchecked
        for (uint32_t p : d1) memcpy(g_sh + (size_t)p * PG, g_ar + (size_t)p * PG, PG);
        st.kept++;
        g_kept_total++;
        return r0;
    }
    g_after.resize((size_t)d1.size() * PG);
    for (size_t i = 0; i < d1.size(); i++) {
        memcpy(&g_after[i * PG], g_ar + (size_t)d1[i] * PG, PG);
        memcpy(g_ar + (size_t)d1[i] * PG, g_sh + (size_t)d1[i] * PG, PG);
    }
    ww_take();
    snap_statics(g_s1);
    load_statics(g_s0);
    snap_renderer(g_r1, true);                           // the renderer as the original's pass left it
    g_state->after_original();
    // the rewrite
    if (pre) pre(1);
    g_pass = 1;
    g_log[1].clear();
    int k1;
    g_com_phase = 2;
    if (g_chain) {
        chain_on();
        k1 = run_pass(r, 0, a, &r1);
        chain_off();
    } else {
        k1 = run_pass(r, 1, a, &r1);
    }
    g_com_phase = 0;
    DWORD c1 = k1 ? g_fault_code : 0;
    uint32_t a1 = g_fault_at;
    std::vector<uint32_t> d2 = ww_take();
    // compare
    bool same = k0 == k1 && c0 == c1 && (k0 != 0 || r0 == r1) && g_log[0] == g_log[1];
    std::string where;
    std::vector<uint32_t> all;
    std::merge(d1.begin(), d1.end(), d2.begin(), d2.end(), std::back_inserter(all));
    all.erase(std::unique(all.begin(), all.end()), all.end());
    bool state_same = true;
    for (uint32_t p : all) {
        auto it = std::lower_bound(d1.begin(), d1.end(), p);
        const uint8_t* want = (it != d1.end() && *it == p) ? &g_after[(size_t)(it - d1.begin()) * PG] : g_sh + (size_t)p * PG;
        const uint8_t* got = g_ar + (size_t)p * PG;
        if (memcmp(want, got, PG)) {
            uint32_t k = 0;
            while (want[k] == got[k]) k++;
            uint32_t off = p * PG + k;
            char b[200];
            sprintf(b, "arena %s +0x%x: original %02x, rewrite %02x", arena_region(off), off, want[k], got[k]);
            where = b;
            state_same = false;
            break;
        }
    }
    if (state_same) {
        std::vector<uint8_t> s1b;
        snap_statics(s1b);
        if (s1b != g_s1) {
            size_t k = 0;
            while (s1b[k] == g_s1[k]) k++;
            uint32_t base = 0;
            for (auto& g : g_statics) {
                if (k < base + g.n) {
                    uint32_t ad = g.at + (uint32_t)(k - base);
                    char b[200];
                    sprintf(b, "static 0x%08x (%s): original %02x, rewrite %02x", ad, static_name(ad), g_s1[k], s1b[k]);
                    where = b;
                    break;
                }
                base += g.n;
            }
            state_same = false;
        }
    }
    if (k0 == 2 && k1 == 2 && c0 == c1) {
        if (!state_same) st.fault_state++;               // a fault in both: the state at the fault is only noted
    } else if (!state_same) {
        same = false;
    }
    // the renderer: its OpenGL calls, what only the original may do, its objects and state
    std::string glwhere, rwhere, kwhere;
    g_gl_calls += g_glout[0].size();
    if (g_glout[0] != g_glout[1]) {
        size_t k = 0;
        while (k < g_glout[0].size() && k < g_glout[1].size() && g_glout[0][k] == g_glout[1][k]) k++;
        char b[200];
        sprintf(b, "%zu / %zu calls, the first differing is call %zu: %s / %s", g_glout[0].size(), g_glout[1].size(), k,
                k < g_glout[0].size() ? gl_rec_name(g_glout[0][k]).c_str() : "(none)",
                k < g_glout[1].size() ? gl_rec_name(g_glout[1][k]).c_str() : "(none)");
        glwhere = b;
        same = false;
    }
    if (g_keep[1]) {
        kwhere = std::string("the rewrite ") + g_keep[1] + ", the original didn't";
        same = false;
    }
    char rw[200] = "";
    if (g_state->differs(rw, sizeof rw)) {
        rwhere = rw;
        same = false;
    }
    // the protocol's own claims: the rewrite's pass reached no OpenGL function; the original's made each call it
    // recorded exactly once
    std::string pwhere;
    if (gpu::calls[2]) {
        char b[160];
        sprintf(b, "the rewrite's pass reached OpenGL %ld times", gpu::calls[2]);
        pwhere = b;
    } else if (gpu1 != (long)g_glout[0].size()) {
        char b[160];
        sprintf(b, "the original's pass made %ld OpenGL calls and recorded %zu", gpu1, g_glout[0].size());
        pwhere = b;
    }
    g_state->end(false);                                 // the original's results stand
    g_r2.objs = g_r1.objs;                               // the same objects, as they are now
    snap_renderer(g_r2, false);
    if (pwhere.empty()) {
        std::string d = rsnap_diff(g_r1, g_r2);
        if (!d.empty()) pwhere = "after end(), the renderer isn't as the original's pass left it: " + d;
    }
    if (!pwhere.empty()) {
        st.proto++;
        g_proto_total++;
    }
    // every byte the original changed in the game's memory lies in the footprint (unless replay_only)
    std::string fpwhere;
    if (k0 == 0 && !g_fp.replay_only && !fpfault) {
        for (size_t i = 0; i < d1.size() && fpwhere.empty(); i++) {
            if ((size_t)d1[i] * PG < GAME_OFF) continue;
            for (uint32_t k = 0; k < PG; k++) {
                const uint8_t* was = g_sh + (size_t)d1[i] * PG;
                if (g_after[i * PG + k] != was[k] && !in_footprint(g_ar + (size_t)d1[i] * PG + k)) {
                    char b[160];
                    sprintf(b, "arena %s +0x%x", arena_region(d1[i] * PG + k), d1[i] * PG + k);
                    fpwhere = b;
                    break;
                }
            }
        }
        uint32_t base = 0;
        for (auto& g : g_statics) {
            if (!fpwhere.empty()) break;
            for (uint32_t k = 0; k < g.n; k++)
                if (g_s1[base + k] != g_s0[base + k] && !in_footprint((void*)(uintptr_t)(g.at + k))) {
                    char b[160];
                    sprintf(b, "static 0x%08x (%s)", g.at + k, static_name(g.at + k));
                    fpwhere = b;
                    break;
                }
            base += g.n;
        }
        if (!fpwhere.empty()) {
            st.fpbad++;
            g_fp_total++;
        }
    }
    if (k0 == 1) st.panics++;
    if (k0 == 2) st.faults++;
    if (k0 == 3) st.exits++;
    if (!same) {
        st.bad++;
        g_mismatch_total++;
    }
    if ((!same && st.bad <= 4) || (!fpwhere.empty() && st.fpbad <= 2) || (!pwhere.empty() && st.proto <= 4)) {
        printf("  %s %s (check %ld, %s)\n", !same ? "MISMATCH" : !pwhere.empty() ? "PROTOCOL" : "FOOTPRINT", name.c_str(), st.n, g_phase);
        if (k0 != k1 || c0 != c1)
            printf("    outcome: original %d (%08lx at %08x), rewrite %d (%08lx at %08x)\n", k0, c0, a0, k1, c1, a1);
        else if (k0) printf("    both: %d (%08lx at %08x / %08x)\n", k0, c0, a0, a1);
        if (k0 == 0 && k1 == 0 && r0 != r1) printf("    return: original %08llx, rewrite %08llx\n", r0, r1);
        if (g_log[0] != g_log[1]) {
            size_t k = 0;
            while (k < g_log[0].size() && k < g_log[1].size() && g_log[0][k] == g_log[1][k]) k++;
            printf("    stub logs: %zu / %zu entries, first difference at %zu: %08x / %08x\n", g_log[0].size(), g_log[1].size(), k,
                   k < g_log[0].size() ? g_log[0][k] : 0, k < g_log[1].size() ? g_log[1][k] : 0);
        }
        if (!where.empty()) printf("    state: %s\n", where.c_str());
        if (!glwhere.empty()) printf("    OpenGL calls: %s\n", glwhere.c_str());
        if (!kwhere.empty()) printf("    %s\n", kwhere.c_str());
        if (!rwhere.empty()) printf("    %s\n", rwhere.c_str());
        if (!pwhere.empty()) printf("    protocol: %s\n", pwhere.c_str());
        if (!fpwhere.empty()) printf("    the original wrote outside the footprint: %s\n", fpwhere.c_str());
    }
    // carry on from the original's result
    auto in_d1 = [&](uint32_t p) { return std::binary_search(d1.begin(), d1.end(), p); };
    for (uint32_t p : d2)
        if (!in_d1(p)) memcpy(g_ar + (size_t)p * PG, g_sh + (size_t)p * PG, PG);
    for (size_t i = 0; i < d1.size(); i++) {
        memcpy(g_ar + (size_t)d1[i] * PG, &g_after[i * PG], PG);
        memcpy(g_sh + (size_t)d1[i] * PG, &g_after[i * PG], PG);
    }
    ww_take();
    load_statics(g_s1);
    return r0;
}
static std::map<uint32_t, Reg*> g_regs;
static Reg* reg_at(uint32_t at, int nargs) {
    Reg* r = g_regs.count(at) ? g_regs[at] : 0;
    if (!r) { printf("no rewrite registered at %08x\n", at); ExitProcess(2); }
    if (nargs != r->nargs) { printf("%s takes %d arguments, given %d\n", r->name, r->nargs, nargs); ExitProcess(2); }
    return r;
}
static uint64_t ck(uint32_t at, std::initializer_list<uint32_t> args, PrePass pre = 0) {
    uint32_t a[12] = {0};
    int i = 0;
    for (uint32_t v : args) a[i++] = v;
    return check(reg_at(at, i), a, pre);
}
// an original, unchecked (building the world, as the game does)
static uint64_t orig(uint32_t at, std::initializer_list<uint32_t> args) {
    uint32_t a[12] = {0};
    int i = 0;
    for (uint32_t v : args) a[i++] = v;
    return reg_at(at, i)->run(0, at, a, 0);
}

// ---- the world ----------------------------------------------------------------------------------------------------------
static uint32_t g_keep_top, g_world_top;
static uint32_t U(const void* p) { return (uint32_t)(uintptr_t)p; }
#if defined(__GNUC__) && !defined(__clang__)
// (MSVC converts a function pointer to const void* implicitly; GCC doesn't)
template <typename F, typename = std::enable_if_t<std::is_function_v<std::remove_pointer_t<F>>>>
static uint32_t U(F p) { return (uint32_t)(uintptr_t)p; }
#endif
static uint8_t* fill_buf(uint8_t* p, uint32_t n, int fill) {
    for (uint32_t i = 0; i < n; i++) p[i] = fill < 0 ? (uint8_t)rnd() : (uint8_t)fill;
    return p;
}
static uint8_t* wbuf(uint32_t n, int fill = -1) {       // an argument buffer (random, or a fill byte)
    n = (n + 15) & ~15u;
    if (g_world_top + n > WORLD_SIZE) g_world_top = 0;
    uint8_t* p = g_ar + WORLD_OFF + g_world_top;
    g_world_top += n;
    return fill_buf(p, n, fill);
}
static uint8_t* kbuf(uint32_t n, int fill = -1) {       // a wrapper the round keeps (falls back to wbuf when full)
    n = (n + 15) & ~15u;
    if (g_keep_top + n > KEEP_SIZE) return wbuf(n, fill);
    uint8_t* p = g_ar + KEEP_OFF + g_keep_top;
    g_keep_top += n;
    return fill_buf(p, n, fill);
}
struct World {
    uint32_t dd, prim, back, zbuf, d3d, mat, vp;           // the game's own (the statics point at these)
    std::vector<uint32_t> dds;                             // ddraw wrappers
    std::vector<uint32_t> anysurf;                         // every dsurface wrapper (primary, back buffer, z-buffer too)
    std::vector<uint32_t> surfs;                           // dsurface wrappers of surfaces with pixels
    std::vector<uint32_t> texs;                            // dtexture wrappers
    std::vector<uint32_t> d3ds, mats, vps;                 // d3d, dmaterial, dviewport wrappers
} W;
static uint32_t pick(const std::vector<uint32_t>& v) { return v.empty() ? 0 : v[rnd() % v.size()]; }
static uint32_t rbit() { return rnd() & 1; }
static uint32_t rbyte() { return chance(80) ? rbit() : rnd() & 0xff; }
static uint32_t word(uint32_t a, int i) { return ((uint32_t*)(uintptr_t)a)[i]; }
static bool has_pixels(gfx::Surface* s) { return !s->pixels.empty(); }

// adopting what a call made: the objects held (freed at the round's end), the wrappers in the world's lists
static void adopt_surface_wrapper(uint32_t w, bool texture, bool owned) {
    if (!w || !word(w, 0)) return;
    gfx::Surface* s = gfx::surface((void*)(uintptr_t)word(w, 0));
    if (!owned) hold_obj(s);
    W.anysurf.push_back(w);
    if (has_pixels(s)) W.surfs.push_back(w);
    if (texture) W.texs.push_back(w);
}
static void adopt(std::vector<uint32_t>& list, uint32_t w, gfx::Obj* o) {
    if (!w || !o) return;
    hold_obj(o);
    list.push_back(w);
}

// surface descriptions as the game fills them
static DDSURFACEDESC desc_texture() {
    DDSURFACEDESC d = {};
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = 1u << ri(0, 8);
    d.dwHeight = chance(70) ? d.dwWidth : 1u << ri(0, 8);
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | (chance(50) ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
    if (chance(90)) {
        static const gfx::Format f[] = {gfx::F565, gfx::F555, gfx::F1555, gfx::F4444};
        d.dwFlags |= DDSD_PIXELFORMAT;
        gfx::pixel_format(f[rnd() % 4], d.ddpfPixelFormat);
    }
    if (chance(40)) {
        d.dwFlags |= DDSD_MIPMAPCOUNT;
        d.dwMipMapCount = ri(1, 9);
        d.ddsCaps.dwCaps |= DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
    }
    if (chance(35)) {
        d.dwFlags |= DDSD_CKSRCBLT;
        d.ddckCKSrcBlt.dwColorSpaceLowValue = d.ddckCKSrcBlt.dwColorSpaceHighValue = chance(50) ? 0 : rnd() & 0xffff;
    }
    return d;
}
static DDSURFACEDESC desc_offscreen() {
    DDSURFACEDESC d = {};
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = ri(1, 300);
    d.dwHeight = ri(1, 300);
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    if (chance(50)) {
        d.dwFlags |= DDSD_PIXELFORMAT;
        gfx::pixel_format(chance(50) ? gfx::F565 : gfx::F555, d.ddpfPixelFormat);
    }
    return d;
}
static DDSURFACEDESC desc_zbuffer() {
    DDSURFACEDESC d = {};
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_ZBUFFERBITDEPTH;
    d.dwWidth = gfx::st.w;
    d.dwHeight = gfx::st.h;
    d.dwZBufferBitDepth = 16;
    d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    return d;
}
static DDSURFACEDESC desc_primary(bool flip) {
    DDSURFACEDESC d = {};
    d.dwSize = sizeof d;
    d.dwFlags = DDSD_CAPS | (flip ? DDSD_BACKBUFFERCOUNT : 0);
    d.dwBackBufferCount = flip ? 1 : 0;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | (flip ? DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE : 0);
    return d;
}
static uint32_t in_arena_desc(const DDSURFACEDESC& d) {
    uint8_t* p = wbuf(sizeof d);
    memcpy(p, &d, sizeof d);
    return U(p);
}
static uint32_t rdesc_any() {
    int k = ri(0, 99);
    return in_arena_desc(k < 55 ? desc_texture() : k < 80 ? desc_offscreen() : k < 90 ? desc_zbuffer() : desc_primary(chance(70)));
}
static uint32_t rdesc_tex() { return in_arena_desc(chance(90) ? desc_texture() : desc_offscreen()); }

// the game's 2D and its image loads: pixels written between a lock and an unlock (or any time: they're the game's)
static void paint(gfx::Surface* s) {
    if (s->is_back) {
        if (!gfx::st.page_locked || gfx::pg.page.empty()) return;
        size_t n = gfx::pg.page.size(), at = rnd() % n, len = rnd() % 4000;
        for (size_t i = 0; i < len && at + i < n; i++) gfx::pg.page[at + i] = (uint16_t)rnd();
        return;
    }
    for (size_t i = 0; i < s->pixels.size(); i++) s->pixels[i] = (uint16_t)(chance(20) && s->keyed ? s->key : rnd());
}
static void paint_levels(gfx::Surface* s) {
    for (; s; s = s->next_mip) paint(s);
}

static const int k_modes[][2] = {{640, 480}, {640, 480}, {512, 384}, {800, 600}, {1024, 768}, {320, 240}, {400, 300}};
static const int k_windows[][2] = {{1920, 1080}, {640, 480}, {2560, 1440}, {1280, 720}, {800, 600}, {1024, 768},
                                   {3840, 2160}, {1366, 768}, {600, 800}, {1600, 1200}};
static void random_window() {
    int k = (int)(rnd() % (sizeof k_windows / sizeof k_windows[0]));
    gpu::dw = k_windows[k][0], gpu::dh = k_windows[k][1];
}

static uint32_t rmaterial() {                            // a D3DMATERIAL (0x50)
    float* m = (float*)wbuf(0x50);
    ((uint32_t*)m)[0] = 0x50;
    for (int i = 1; i < 17; i++) m[i] = (float)(rnd() % 1000) / 1000.0f;
    return U(m);
}
static uint32_t rmatrix() {
    float* m = (float*)wbuf(0x40);
    for (int i = 0; i < 16; i++) m[i] = chance(3) ? 0.0f : ((float)(int)(rnd() % 2001) - 1000.0f) / 250.0f;
    return U(m);
}

static bool g_started;
static void reset_world(int round) {
    free_world_objects();
    memset(g_ar, 0, HEAP_OFF);
    Ctrl* c = C();
    c->fr = rnd() | 1;
    c->heap_top = HEAP_OFF;
    c->oom_pct = 0;
    g_keep_top = g_world_top = 0;
    gpu::rng = rnd() | 1;
    random_window();
    if (!g_started) {
        gpu::install();
        gfx::check_hooks_install();                      // gl_core.cpp's ShadowState -> shadow_set_state
        if (!gfx::start_headless()) { printf("the renderer didn't start\n"); ExitProcess(2); }
        if (dxgi::active() || gl_api.SwapWindow != &gpu::swap_window) {   // the DXGI present stays off headless
            printf("the DXGI present started headless\n");
            ExitProcess(2);
        }
        g_started = true;
    }
    W = World();
    M32(H_D_DEVICE) = 0;
    M32(H_X_RESULT) = 0;
    *(uint8_t*)(uintptr_t)H_V_HAS3DHW = (uint8_t)chance(85);
    // the start-up, by the originals, as the game makes it (vid.obj's grab_ddraw / make_primary ... dx.obj's)
    W.dd = (uint32_t)orig(0x0045e950, {0});                                   // dd_create
    adopt(W.dds, W.dd, gfx::directdraw((void*)(uintptr_t)word(W.dd, 0)));
    orig(0x0045f200, {W.dd, 0});                                              // ddraw::set_exclusive
    int m = (int)(rnd() % (sizeof k_modes / sizeof k_modes[0]));
    orig(0x0045f270, {W.dd, 0, (uint32_t)k_modes[m][0], (uint32_t)k_modes[m][1], 16});   // ddraw::set_mode
    W.prim = (uint32_t)orig(0x0045f3a0, {W.dd, 0, in_arena_desc(desc_primary(true))});   // ddraw::create_surface
    adopt_surface_wrapper(W.prim, false, false);
    uint32_t* caps = (uint32_t*)wbuf(4);
    caps[0] = DDSCAPS_BACKBUFFER;
    W.back = (uint32_t)orig(0x0045f5d0, {W.prim, 0, U(caps)});               // dsurface::get_attached
    adopt_surface_wrapper(W.back, false, true);
    W.zbuf = (uint32_t)orig(0x0045f3a0, {W.dd, 0, in_arena_desc(desc_zbuffer())});
    adopt_surface_wrapper(W.zbuf, false, false);
    orig(0x0045f640, {W.back, 0, W.zbuf});                                    // dsurface::add_attached
    W.d3d = U(kbuf(8, 0xa3));
    orig(0x0045ed90, {W.d3d, 0, W.dd});                                       // d3d::d3d
    adopt(W.d3ds, W.d3d, gfx::direct3d((void*)(uintptr_t)word(W.d3d, 0)));
    orig(0x0045edd0, {W.d3d, 0, W.back});                                     // d3d::connect
    hold_obj(gfx::device((void*)(uintptr_t)word(W.d3d, 1)));
    M32(H_X_D3D) = W.d3d;
    // textures: pairs made as the game makes them (a system-memory image loaded into a video texture), and others
    int nt = ri(3, 8);
    for (int i = 0; i < nt; i++) {
        DDSURFACEDESC d = desc_texture();
        uint32_t t = (uint32_t)orig(0x0045f400, {W.dd, 0, in_arena_desc(d)});  // ddraw::create_texture_surface
        adopt_surface_wrapper(t, true, false);
        if (!t) continue;
        paint_levels(gfx::surface((void*)(uintptr_t)word(t, 0)));
        if (chance(60)) {                                // its video copy, loaded (the handle set)
            d.ddsCaps.dwCaps = (d.ddsCaps.dwCaps & ~DDSCAPS_SYSTEMMEMORY) | DDSCAPS_VIDEOMEMORY;
            uint32_t v = (uint32_t)orig(0x0045f400, {W.dd, 0, in_arena_desc(d)});
            adopt_surface_wrapper(v, true, false);
            if (v) orig(0x0045f7e0, {v, 0, t});                              // dtexture::load
        }
    }
    int no = ri(1, 3);
    for (int i = 0; i < no; i++) {
        uint32_t s = (uint32_t)orig(0x0045f3a0, {W.dd, 0, in_arena_desc(desc_offscreen())});
        adopt_surface_wrapper(s, false, false);
        if (s) paint(gfx::surface((void*)(uintptr_t)word(s, 0)));
    }
    int nm = ri(1, 3);
    for (int i = 0; i < nm; i++) {
        uint32_t w = (uint32_t)orig(0x0045ef20, {W.d3d, 0, rmaterial()});     // d3d::CreateMaterial
        if (w) adopt(W.mats, w, gfx::material((void*)(uintptr_t)word(w, 0)));
    }
    W.mat = W.mats[0];
    int nv = ri(1, 2);
    for (int i = 0; i < nv; i++) {
        uint32_t w = (uint32_t)orig(0x0045ef70, {W.d3d, 0, pick(W.mats)});    // d3d::CreateViewport (made current)
        if (w) adopt(W.vps, w, gfx::viewport((void*)(uintptr_t)word(w, 0)));
    }
    W.vp = W.vps[0];
    orig(0x0045ec20, {W.vp, 0, 0, 0, (uint32_t)gfx::st.w, (uint32_t)gfx::st.h});   // dviewport::set_viewport
    C()->oom_pct = round % 7 == 6 ? 10 : 1;
    ww_sync();
}

// the statics back on the world's d3d and device before each call; dx_result scribbled (a missed store shows)
static void heal() {
    M32(H_X_D3D) = W.d3d;
    M32(H_D_DEVICE) = word(W.d3d, 1);
    if (chance(70)) M32(H_X_RESULT) = chance(50) ? rnd() : 0x88760000u + (rnd() & 0x3ff);
    if (chance(20)) *(uint8_t*)(uintptr_t)H_V_HAS3DHW = (uint8_t)chance(85);
}

// ---- objects made to be destroyed (never used again: a survivor is left to leak) --------------------------------------
static gfx::Surface* new_surface(const DDSURFACEDESC& d0) {
    DDSURFACEDESC d = d0;
    gfx::Surface* s = 0;
    gfx::create_surface(&d, &s);
    return s;
}
static uint32_t wrap(void* h, uint32_t bytes = 4) {
    uint32_t* w = (uint32_t*)wbuf(bytes);
    w[0] = U(h);
    return U(w);
}
static uint32_t victim_surface() {
    int k = ri(0, 9);
    gfx::Surface* s = new_surface(k < 6 ? desc_offscreen() : k < 9 ? desc_texture() : desc_primary(true));
    if (chance(20)) s->hold();                           // not its last reference
    return wrap(gfx::handle(s));
}
static uint32_t victim_texture() {                       // as ddraw::create_texture_surface leaves it: two references
    gfx::Surface* s = new_surface(desc_texture());
    s->hold();
    if (chance(15)) s->hold();
    uint32_t* t = (uint32_t*)wbuf(16);
    t[0] = U(gfx::handle(s));
    t[1] = U(gfx::handle(&s->t2));
    t[2] = rnd();
    t[3] = chance(50) ? t[0] : 0;
    return U(t);
}
static uint32_t victim_material() {
    gfx::Material* m = gfx::make<gfx::Material>();
    uint32_t* w = (uint32_t*)wbuf(8);
    w[0] = U(gfx::handle(m));
    w[1] = gfx::material_handle(m);
    return U(w);
}
static uint32_t victim_viewport() {
    gfx::Viewport* v = gfx::make<gfx::Viewport>();
    if (M32(H_D_DEVICE) && chance(40)) gfx::use_viewport(gfx::device((void*)(uintptr_t)M32(H_D_DEVICE)), v);   // current
    return wrap(gfx::handle(v));
}
static uint32_t victim_d3d() {
    uint32_t* w = (uint32_t*)wbuf(8);
    w[0] = U(gfx::handle(gfx::make<gfx::Direct3D>()));
    w[1] = word(W.d3d, 1);
    return U(w);
}
static uint32_t victim_device() {                        // d3d::disconnect's: the world's Direct3D, a device of its own
    uint32_t* w = (uint32_t*)wbuf(8);
    w[0] = word(W.d3d, 0);
    w[1] = U(gfx::handle(gfx::make<gfx::Device>()));
    return U(w);
}
static uint32_t victim_ddraw() { return wrap(gfx::handle(gfx::make<gfx::DirectDraw2>())); }

// ---- draws ------------------------------------------------------------------------------------------------------------------
// The vertices live outside the arena (the game builds them in locals): before each pass the fields Direct3D never reads
// -- an LVERTEX's reserved dword, and u/v with no texture bound -- get different garbage, as two stack frames would.
static uint8_t g_vtx[512 * 32];
static uint16_t g_idx[1024];
static struct { uint32_t n, vtype; bool textured; } g_draw;
static long g_draws, g_draws_textured;
static void draw_garbage(int) {
    for (uint32_t i = 0; i < g_draw.n; i++) {
        uint32_t* v = (uint32_t*)(g_vtx + i * 32);
        if (g_draw.vtype == 2) v[3] = rnd();
        if (!g_draw.textured) v[6] = rnd(), v[7] = rnd();
    }
}
static bool textured_now() {
    gfx::Surface* t = (gfx::Surface*)(uintptr_t)gfx::st.rs[D3DRENDERSTATE_TEXTUREHANDLE];
    return t && gfx::handles.count(t);
}
static void make_vertices(uint32_t n, uint32_t vtype) {
    for (uint32_t i = 0; i < n; i++) {
        float* f = (float*)(g_vtx + i * 32);
        uint32_t* u = (uint32_t*)f;
        for (int k = 0; k < 3; k++) f[k] = ((float)(int)(rnd() % 20001) - 10000.0f) / (vtype == 3 ? 20.0f : 1000.0f);
        if (vtype == 3) f[0] = fabsf(f[0]) * 1.3f, f[1] = fabsf(f[1]), f[2] = (float)(rnd() % 1000) / 1000.0f,
                        f[3] = (float)(rnd() % 1000 + 1) / 1000.0f;
        u[4] = rnd();
        u[5] = rnd();
        f[6] = (float)(int)(rnd() % 4001) / 1000.0f - 2.0f;
        f[7] = (float)(int)(rnd() % 4001) / 1000.0f - 2.0f;
        if (chance(1)) u[rnd() % 8] = rnd();              // now and then anything at all (a NaN, a denormal)
    }
    g_draw.n = n;
    g_draw.vtype = vtype;
    g_draw.textured = textured_now();
    g_draws++;
    g_draws_textured += g_draw.textured;
}
static uint32_t make_indices(uint32_t ni, uint32_t nv) {
    for (uint32_t i = 0; i < ni; i++) g_idx[i] = (uint16_t)(nv ? rnd() % nv : 0);
    return U(g_idx);
}
static void bind_texture() {                             // half the time a loaded texture is bound
    if (chance(50)) {
        uint32_t t = pick(W.texs);
        if (t && word(t, 0)) {
            gfx::Surface* s = gfx::surface((void*)(uintptr_t)word(t, 0));
            gfx::set_state(D3DRENDERSTATE_TEXTUREHANDLE, gfx::tex_handle(s->root ? s->root : s));
        }
    } else if (chance(50)) {
        gfx::set_state(D3DRENDERSTATE_TEXTUREHANDLE, 0);
    }
}
static void draw(uint32_t at) {
    if (chance(60)) bind_texture();
    uint32_t vtype = at == 0x00458be0 ? 3 : 2;
    switch (at) {
    case 0x00458b70:                                     // dxDrawTriangle
        make_vertices(3, vtype);
        ck(at, {U(g_vtx)}, draw_garbage);
        break;
    case 0x00458b90: {                                   // dxDrawTriangles
        uint32_t n = (uint32_t)ri(0, 60) / 3 * 3;
        make_vertices(n, vtype);
        ck(at, {U(g_vtx), n}, draw_garbage);
        break;
    }
    default: {                                           // dxDPDraw, dxDrawIndexTriangles, dxDrawScreenIndexTriangles
        uint32_t nv = (uint32_t)ri(0, 100), ni = (uint32_t)ri(0, 60) / 3 * 3;
        if (chance(3)) nv = (uint32_t)ri(100, 512), ni = (uint32_t)ri(0, 1024) / 3 * 3;
        make_vertices(nv, vtype);
        ck(at, {U(g_vtx), nv, make_indices(ni, nv), ni}, draw_garbage);
        break;
    }
    }
}

// ---- the calls: every function, with arguments from the world --------------------------------------------------------
static uint32_t rxform(uint32_t n) {                     // a D3DTRANSFORMDATA over world buffers
    uint32_t* x = (uint32_t*)wbuf(0x34);
    x[0] = 0x34;
    float* in = (float*)wbuf(n * 0x20);
    for (uint32_t i = 0; i < n * 8; i++) in[i] = ((float)(int)(rnd() % 20001) - 10000.0f) / 500.0f;
    x[1] = U(in);
    x[2] = 0x20;
    x[3] = U(wbuf(n * 0x20));
    x[4] = 0x20;
    x[5] = chance(80) ? U(wbuf(n * 0x10)) : 0;
    x[6] = 0x3f;
    return U(x);
}
static uint32_t rsurfdesc() {                            // a DDSURFACEDESC to fill: dwSize the game's (or 0)
    uint32_t* d = (uint32_t*)wbuf(0x6c);
    d[0] = chance(90) ? 0x6c : 0;
    return U(d);
}
static uint32_t rcaps(uint32_t size) {                  // a DDCAPS / D3DDEVICEDESC to fill (now and then none)
    if (chance(8)) return 0;
    uint32_t* d = (uint32_t*)wbuf(size);
    d[0] = chance(95) ? size : 0;
    return U(d);
}
static uint32_t rrect() {
    int32_t* r = (int32_t*)wbuf(16);
    r[0] = ri(0, 200), r[1] = ri(0, 200);
    r[2] = r[0] + ri(-5, 200), r[3] = r[1] + ri(-5, 200);
    return U(r);
}
static uint32_t rstate() { return chance(95) ? (uint32_t)ri(0, 63) : (uint32_t)ri(64, 200); }
static uint32_t rvalue(uint32_t s) {
    if (s == D3DRENDERSTATE_TEXTUREHANDLE) {
        uint32_t t = pick(W.texs);
        return chance(70) && t ? word(t, 2) : rnd();
    }
    if (s == D3DRENDERSTATE_MIPMAPLODBIAS || s == D3DRENDERSTATE_FOGCOLOR) return rnd();
    return chance(85) ? (uint32_t)ri(0, 8) : rnd();
}
// a d3d wrapper with a device: the originals of GetCaps ... EnumTextureFormats call it (the game calls them connected)
static uint32_t pick_connected() {
    for (int i = 0; i < 8; i++) {
        uint32_t d = pick(W.d3ds);
        if (word(d, 1)) return d;
    }
    return W.d3d;
}
static uint32_t pixel_surf() {                           // a dsurface wrapper with pixels (a texture's too)
    return chance(70) || W.texs.empty() ? pick(W.surfs) : pick(W.texs);
}

static void call(uint32_t at) {
    uint64_t r;
    switch (at) {
    // dd.obj
    case 0x0045e940: ck(at, {chance(50) ? 0x00454ac0u : U(&h_ddenum_cb)}); break;   // dd_enum
    case 0x0045e950:                                                                  // dd_create
        r = ck(at, {chance(30) ? 0 : U(wbuf(16))});
        if (r && word((uint32_t)r, 0)) adopt(W.dds, (uint32_t)r, gfx::directdraw((void*)(uintptr_t)word((uint32_t)r, 0)));
        break;
    case 0x0045ea20: ck(at, {chance(10) ? 0 : victim_ddraw()}); break;               // dd_destroy
    case 0x0045ea40: {                                                                // dmaterial::dmaterial
        uint32_t self = U(kbuf(8, 0xa3));
        ck(at, {self, 0, word(pick(W.d3ds), 0), rmaterial()});
        if (word(self, 0)) adopt(W.mats, self, gfx::material((void*)(uintptr_t)word(self, 0)));
        break;
    }
    case 0x0045eaf0: ck(at, {victim_material(), 0}); break;
    case 0x0045eb10: ck(at, {pick(W.mats), 0, rmaterial()}); break;
    case 0x0045eb40: {                                                                // dviewport::dviewport
        uint32_t self = U(kbuf(4, 0xa3));
        ck(at, {self, 0, word(pick(W.d3ds), 0), pick(W.mats)});
        if (word(self, 0)) adopt(W.vps, self, gfx::viewport((void*)(uintptr_t)word(self, 0)));
        break;
    }
    case 0x0045ebf0: ck(at, {victim_viewport(), 0}); break;
    case 0x0045ec20: {                                                                // dviewport::set_viewport
        uint32_t w = chance(5) ? 0 : chance(50) ? (uint32_t)gfx::st.w : (uint32_t)ri(1, 1024);
        uint32_t h = chance(50) ? (uint32_t)gfx::st.h : (uint32_t)ri(0, 768);
        if (g_vrmod) {
            vrmod_aspect();
            if (chance(20)) w = 640, h = 416;                                        // aspectfix's own design point
        }
        ck(at, {pick(W.vps), 0, (uint32_t)ri(0, 64), (uint32_t)ri(0, 64), w, h});
        break;
    }
    case 0x0045ece0: ck(at, {pick(W.vps), 0, (uint32_t)ri(0, 640), (uint32_t)ri(0, 480), (uint32_t)ri(0, 640), (uint32_t)ri(0, 480), rbyte()}); break;
    case 0x0045ed50: {                                                                // dviewport::transform_verts
        uint32_t n = (uint32_t)ri(1, 4);
        ck(at, {pick(W.vps), 0, n, rxform(n), (uint32_t)ri(0, 3), chance(90) ? U(wbuf(4)) : 0});
        break;
    }
    case 0x0045ed90: {                                                                // d3d::d3d
        uint32_t self = U(kbuf(8, 0xa3));
        ck(at, {self, 0, pick(W.dds)});
        if (word(self, 0)) adopt(W.d3ds, self, gfx::direct3d((void*)(uintptr_t)word(self, 0)));
        break;
    }
    case 0x0045edb0: ck(at, {victim_d3d(), 0}); break;                               // d3d::~d3d
    case 0x0045edd0: {                                                                // d3d::connect
        uint32_t d = chance(30) ? W.d3d : pick(W.d3ds);
        ck(at, {d, 0, pick(W.anysurf)});
        if (word(d, 1)) hold_obj(gfx::device((void*)(uintptr_t)word(d, 1)));
        break;
    }
    case 0x0045ee00: ck(at, {victim_device(), 0}); break;                            // d3d::disconnect
    case 0x0045ee20:                                                                  // d3d::create_driver
        r = ck(at, {pick(W.d3ds), 0, pick(W.dds)});
        if (r) hold_obj(gfx::direct3d((void*)(uintptr_t)r));
        break;
    case 0x0045ee70:                                                                  // d3d::create_device
        r = ck(at, {pick(W.d3ds), 0, pick(W.anysurf)});
        if (r) hold_obj(gfx::device((void*)(uintptr_t)r));
        break;
    case 0x0045ef20:                                                                  // d3d::CreateMaterial
        r = ck(at, {pick(W.d3ds), 0, rmaterial()});
        if (r && word((uint32_t)r, 0)) adopt(W.mats, (uint32_t)r, gfx::material((void*)(uintptr_t)word((uint32_t)r, 0)));
        break;
    case 0x0045ef50: ck(at, {pick(W.d3ds), 0, chance(15) ? 0 : victim_material()}); break;
    case 0x0045ef70:                                                                  // d3d::CreateViewport
        r = ck(at, {pick(W.d3ds), 0, pick(W.mats)});
        if (r && word((uint32_t)r, 0)) adopt(W.vps, (uint32_t)r, gfx::viewport((void*)(uintptr_t)word((uint32_t)r, 0)));
        break;
    case 0x0045efa0: ck(at, {pick(W.d3ds), 0, chance(15) ? 0 : victim_viewport()}); break;
    case 0x0045efc0: ck(at, {pick_connected(), 0, rcaps(0xcc), rcaps(0xcc)}); break;     // d3d::GetCaps
    case 0x0045f000: {                                                                // d3d::GetStats
        uint32_t* st = (uint32_t*)wbuf(0x18);
        st[0] = chance(90) ? 0x18 : 0;
        ck(at, {pick_connected(), 0, U(st)});
        break;
    }
    case 0x0045f040: case 0x0045f070: ck(at, {pick_connected(), 0}); break;
    case 0x0045f0a0: case 0x0045f0d0: {                                               // SetRenderState / SetLightState
        uint32_t s = rstate();
        ck(at, {pick_connected(), 0, s, rvalue(s)});
        break;
    }
    case 0x0045f100: ck(at, {pick_connected(), 0, chance(90) ? (uint32_t)ri(1, 3) : (uint32_t)ri(0, 8), rmatrix()}); break;
    case 0x0045f130: ck(at, {pick_connected(), 0, U(&h_texfmt_cb), U(wbuf(4))}); break;
    // ddraw
    case 0x0045f160: {                                                                // ddraw::ddraw
        uint32_t self = U(kbuf(4, 0xa3));
        ck(at, {self, 0, word(pick(W.dds), 0)});
        W.dds.push_back(self);
        break;
    }
    case 0x0045f170: ck(at, {victim_ddraw(), 0}); break;
    case 0x0045f190: ck(at, {pick(W.dds), 0, rcaps(0x16c), rcaps(0x16c)}); break;
    case 0x0045f1c0: {                                                                // ddraw::get_mem
        uint32_t* caps = (uint32_t*)wbuf(4);
        caps[0] = chance(50) ? (chance(50) ? DDSCAPS_NONLOCALVIDMEM : DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY) : rnd();
        ck(at, {pick(W.dds), 0, chance(5) ? 0 : U(caps), chance(10) ? 0 : U(wbuf(4)), chance(10) ? 0 : U(wbuf(4))});
        break;
    }
    case 0x0045f200: case 0x0045f2c0: ck(at, {pick(W.dds), 0}); break;
    case 0x0045f240: ck(at, {pick(W.dds), 0, U(wbuf(0x6c)), chance(50) ? 0x00455330u : U(&h_modes_cb)}); break;
    case 0x0045f270: {                                                                // ddraw::set_mode
        int m = (int)(rnd() % (sizeof k_modes / sizeof k_modes[0]));
        uint32_t w = chance(80) ? (uint32_t)k_modes[m][0] : (uint32_t)ri(16, 1024);
        uint32_t h = chance(80) ? (uint32_t)k_modes[m][1] : (uint32_t)ri(16, 768);
        if (chance(20)) random_window();
        ck(at, {pick(W.dds), 0, w, h, chance(80) ? 16u : 32u});
        break;
    }
    case 0x0045f2d0:                                                                  // ddraw::make_dd_surface
        r = ck(at, {pick(W.dds), 0, rdesc_any()});
        if (r) {                                         // (a dsurface of the harness's around it, to use it again)
            uint32_t w = U(kbuf(4));
            *(uint32_t*)(uintptr_t)w = (uint32_t)r;
            adopt_surface_wrapper(w, false, false);
        }
        break;
    case 0x0045f3a0: adopt_surface_wrapper((uint32_t)ck(at, {pick(W.dds), 0, rdesc_any()}), false, false); break;
    case 0x0045f400: {                                                                // ddraw::create_texture_surface
        uint32_t t = (uint32_t)ck(at, {pick(W.dds), 0, rdesc_tex()});
        adopt_surface_wrapper(t, true, false);
        if (t && chance(70)) paint_levels(gfx::surface((void*)(uintptr_t)word(t, 0)));
        break;
    }
    case 0x0045f3e0: ck(at, {pick(W.dds), 0, chance(15) ? 0 : victim_surface()}); break;
    case 0x0045f440: ck(at, {pick(W.dds), 0, chance(15) ? 0 : victim_texture()}); break;
    // dsurface
    case 0x0045f460: {                                                                // dsurface::dsurface
        uint32_t self = U(kbuf(4, 0xa3)), from = pick(W.anysurf);
        ck(at, {self, 0, word(from, 0)});
        adopt_surface_wrapper(self, false, true);
        break;
    }
    case 0x0045f470: ck(at, {victim_surface(), 0}); break;
    case 0x0045f490: {                                                                // dsurface::lock (then the 2D)
        uint32_t w = chance(40) ? W.back : chance(80) ? pixel_surf() : pick(W.anysurf);
        ck(at, {w, 0, rsurfdesc()});
        if (chance(80)) paint(gfx::surface((void*)(uintptr_t)word(w, 0)));
        break;
    }
    case 0x0045f510: ck(at, {chance(40) ? W.back : chance(80) ? pixel_surf() : pick(W.anysurf), 0}); break;
    case 0x0045f520:                                                                  // dsurface::flip
        if (chance(5)) random_window();                  // the window resized: the target follows
        ck(at, {chance(80) ? W.prim : pick(W.anysurf), 0});
        break;
    case 0x0045f580: case 0x0045f670: ck(at, {pick(W.anysurf), 0}); break;
    case 0x0045f5a0: ck(at, {pick(W.anysurf), 0, rsurfdesc()}); break;
    case 0x0045f5d0: {                                                                // dsurface::get_attached
        uint32_t* c = (uint32_t*)wbuf(4);
        c[0] = chance(60) ? DDSCAPS_BACKBUFFER : chance(60) ? DDSCAPS_TEXTURE | DDSCAPS_MIPMAP : rnd();
        uint32_t from = chance(30) ? W.prim : chance(50) ? pick(W.texs) : pick(W.anysurf);
        adopt_surface_wrapper((uint32_t)ck(at, {from, 0, U(c)}), false, true);
        break;
    }
    case 0x0045f640: ck(at, {pick(W.anysurf), 0, pick(W.anysurf)}); break;
    case 0x0045f690: ck(at, {pixel_surf(), 0, pixel_surf()}); break;                 // dsurface::blit
    case 0x0045f6d0: ck(at, {pixel_surf(), 0, pixel_surf(), (uint32_t)ri(0, 300), (uint32_t)ri(0, 300), chance(30) ? 0 : rrect()}); break;
    case 0x0045f710: ck(at, {chance(80) ? pixel_surf() : pick(W.anysurf), 0, chance(30) ? 0 : rnd() & 0xffff}); break;
    // dtexture
    case 0x0045f750: {                                                                // dtexture::dtexture
        uint32_t self = U(kbuf(16, 0xa3)), from = pixel_surf();
        ck(at, {self, 0, word(from, 0)});
        adopt_surface_wrapper(self, true, true);
        break;
    }
    case 0x0045f7c0: ck(at, {victim_texture(), 0}); break;
    case 0x0045f7e0: ck(at, {pick(W.texs), 0, pick(W.texs)}); break;                  // dtexture::load
    case 0x0045f870: case 0x0045f930: case 0x0045f940: ck(at, {pick(W.texs), 0}); break;
    case 0x0045f9a0: {                                                                // dtexture::cleanup_mip
        uint32_t t = pick(W.texs);
        uint32_t* w = (uint32_t*)(uintptr_t)t;
        if (!w[3]) w[3] = w[0];                          // (the game walks from first_mip: the first level is set)
        ck(at, {t, 0});
        break;
    }
    case 0x0045f8a0: ck(at, {pick(W.texs), 0, rbyte(), rbyte()}); break;
    // dx.obj's draws
    case 0x00458a60: case 0x00458b70: case 0x00458b90: case 0x00458bb0: case 0x00458be0: draw(at); break;
    default:
        printf("no generator for %08x\n", at);
        ExitProcess(2);
    }
}

// ---- rounds -------------------------------------------------------------------------------------------------------------
static std::vector<uint32_t> g_addrs;
static void round_all(int r) {
    reset_world(r);
    std::vector<uint32_t> order = g_addrs;
    for (size_t i = order.size(); i > 1; i--) std::swap(order[i - 1], order[rnd() % i]);
    for (uint32_t at : order) {
        heal();
        call(at);
        if (chance(2)) reset_world(r);
    }
}
// the calls a frame makes, in the frame's order, on one world
static void round_frame(int r) {
    reset_world(r);
    static const uint32_t draws[] = {0x00458a60, 0x00458a60, 0x00458bb0, 0x00458be0, 0x00458b70, 0x00458b90};
    for (int f = 0; f < 5; f++) {
        heal();
        call(0x0045f040);                                  // d3d::BeginFrame
        if (chance(50)) { heal(); ck(0x0045ece0, {W.vp, 0, 0, 0, (uint32_t)gfx::st.w, (uint32_t)gfx::st.h, rbyte()}); }
        for (int s = 0; s < 10; s++) {
            heal();
            int k = ri(0, 9);
            if (k < 3) call(0x0045f0a0);                   // SetRenderState
            else if (k < 5) call(chance(50) ? 0x0045f870 : 0x0045f8a0);   // dtexture::select
            else if (k < 6) call(0x0045f100);              // SetTransform
            heal();
            draw(draws[rnd() % 6]);
        }
        heal();
        call(0x0045f070);                                  // d3d::EndFrame
        heal();
        ck(0x0045f490, {W.back, 0, rsurfdesc()});          // the 2D on the page
        paint(gfx::surface((void*)(uintptr_t)word(W.back, 0)));
        heal();
        ck(0x0045f510, {W.back, 0});
        heal();
        call(0x0045f520);                                  // dsurface::flip
    }
}
// the start-up, checked, in the game's order
static void round_setup(int r) {
    reset_world(r);
    static const uint32_t seq[] = {0x0045e940, 0x0045e950, 0x0045f190, 0x0045f1c0, 0x0045f200, 0x0045f240, 0x0045f270,
                                   0x0045f3a0, 0x0045f5d0, 0x0045f3a0, 0x0045f640, 0x0045ed90, 0x0045edd0, 0x0045efc0,
                                   0x0045f130, 0x0045f400, 0x0045f400, 0x0045f7e0, 0x0045ef20, 0x0045ef70, 0x0045ec20,
                                   0x0045eb10, 0x0045f000, 0x0045f2c0};
    for (uint32_t at : seq) {
        heal();
        call(at);
    }
}
// the texture and page paths, many times each
static void round_focus(int r) {
    reset_world(r);
    static const uint32_t fns[] = {0x0045f490, 0x0045f510, 0x0045f690, 0x0045f6d0, 0x0045f710, 0x0045f7e0, 0x0045f870,
                                   0x0045f8a0, 0x0045f930, 0x0045f940, 0x0045f9a0, 0x0045ec20, 0x0045ed50, 0x0045ece0,
                                   0x0045f520, 0x0045f270, 0x0045f400, 0x00458a60, 0x00458be0};
    for (int i = 0; i < 80; i++) {
        heal();
        call(fns[rnd() % (sizeof fns / sizeof fns[0])]);
    }
    // a mip walk: first, next... (lock and paint each level), cleanup
    for (int i = 0; i < 4; i++) {
        uint32_t t = pick(W.texs);
        heal();
        ck(0x0045f930, {t, 0});
        for (int k = 0; k < 10; k++) {
            heal();
            if (!ck(0x0045f940, {t, 0})) break;
            heal();
            ck(0x0045f490, {t, 0, rsurfdesc()});
            paint(gfx::surface((void*)(uintptr_t)word(t, 0)));
            heal();
            ck(0x0045f510, {t, 0});
        }
        heal();
        ck(0x0045f9a0, {t, 0});
    }
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS* e) {
    printf("UNHANDLED exception %08lx at %08x during %s / %s, pass %d\n", e->ExceptionRecord->ExceptionCode,
           (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress, g_phase, g_phase_name.c_str(), g_pass);
    fflush(stdout);
    ExitProcess(6);
}
// VP_VRMOD=1: vrmod's race.exe (test/vrmod_image.h). Before each check of dviewport::set_viewport, aspectfix's R0 is
// a random player value, and its two addresses (R0, -0.5) now and then point at other .rdata floats, as aspectfix.py
// does when the pool already holds the value (docs/FIXES.md, "vrmod's patches": the rewrite reads both through them).
static void vrmod_aspect() {
    static const uint32_t pool[] = {0x004e0294, 0x004dc39c, 0x004dc3ac, 0x004db054, 0x004e0298};   // slack, 2, 0.5, -0.5, slack
    float r0 = chance(30) ? 0.65f : chance(10) ? 0.0f : (float)ri(200, 1200) / 1000.0f;
    uint32_t bits;
    memcpy(&bits, &r0, 4);
    if (chance(3)) bits = 0x7fc00000u;                                           // NaN: the jbe keeps h/w
    vrmod_put32(0x004e0294, bits);
    vrmod_put32(0x004e0298, chance(50) ? 0xbf000000u : rnd());                  // -0.5, or anything
    vrmod_put32(0x0045ec67, chance(70) ? 0x004e0294 : pool[rnd() % 5]);
    vrmod_put32(0x0045ec88, chance(70) ? 0x004db054 : pool[rnd() % 5]);
}
static int main2(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 40;
    g_rng = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 0x2545f491u;
    if (!g_rng) g_rng = 1;
    const char* modes = argc > 3 ? argv[3] : "both";
    if (!load_race_exe("out\\race_v10.exe")) return 2;
    g_vrmod = getenv("VP_VRMOD") && atoi(getenv("VP_VRMOD"));
    if (g_vrmod) { vrmod_apply(); puts("vrmod's race.exe (test/vrmod_image.h), random aspectfix R0"); }
    g_ar = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    g_sh = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_ww = (ULONG_PTR*)malloc(sizeof(ULONG_PTR) * NPAGES);
    if (!g_ar || !g_sh || !g_ww) { printf("can't allocate the arena\n"); return 2; }
    install_stubs();
    for (Reg* r = Reg::head(); r; r = r->next) {
        g_regs[r->at] = r;
        if (in_group(r->at)) g_addrs.push_back(r->at);
    }
    std::sort(g_addrs.begin(), g_addrs.end());
    ww_sync();
    printf("%zu rewrites in the group; %d rounds, seed 0x%08x\n", g_addrs.size(), rounds, g_rng);
    uint32_t seed = g_rng;
    for (int mode = 0; mode < 2; mode++) {
        if ((mode == 0 && !strcmp(modes, "chain")) || (mode == 1 && !strcmp(modes, "isolated"))) continue;
        g_chain = mode == 1;
        g_rng = seed;
        printf("-- %s --\n", g_chain ? "chain" : "isolated");
        for (int r = 0; r < rounds; r++) {
            g_phase = "all";
            round_all(r);
            g_phase = "frame";
            round_frame(r);
            g_phase = "setup";
            round_setup(r);
            g_phase = "focus";
            round_focus(r);
        }
    }
    free_world_objects();
    printf("\n%-40s %8s %6s %6s %6s %6s %6s %6s %6s %6s %s\n", "function", "checks", "differ", "proto", "kept", "panics",
           "faults", "exits", "ronly", "fp", "(fault-state)");
    std::sort(g_stats.begin(), g_stats.end(), [](const Stat& a, const Stat& b) { return a.name < b.name; });
    for (auto& s : g_stats)
        printf("%-40s %8ld %6ld %6ld %6ld %6ld %6ld %6ld %6ld %6ld %ld\n", s.name.c_str(), s.n, s.bad, s.proto, s.kept, s.panics,
               s.faults, s.exits, s.ronly, s.fpbad, s.fault_state);
    size_t unchecked = 0;
    for (uint32_t at : g_addrs) {
        bool seen = false;
        for (auto& s : g_stats)
            if (s.name == g_regs[at]->name || s.name == std::string(g_regs[at]->name) + " [chain]") seen = true;
        if (!seen) {
            printf("  never checked: %s\n", g_regs[at]->name);
            unchecked++;
        }
    }
    // what the fake OpenGL was asked for, outside the checks' rewrite passes (coverage of the renderer's paths)
    printf("\ncoverage: %ld draws (%ld textured); OpenGL: TexImage2D %ld, TexSubImage2D %ld, ReadPixels %ld, "
           "BlitFramebuffer %ld, DrawArrays %ld, DrawElements %ld, GenFramebuffers %ld, DeleteFramebuffers %ld, "
           "SwapWindow %ld, Clear %ld\n",
           g_draws, g_draws_textured, gpu::by_id[gpu::G_TexImage2D], gpu::by_id[gpu::G_TexSubImage2D],
           gpu::by_id[gpu::G_ReadPixels], gpu::by_id[gpu::G_BlitFramebuffer], gpu::by_id[gpu::G_DrawArrays],
           gpu::by_id[gpu::G_DrawElements], gpu::by_id[gpu::G_GenFramebuffers], gpu::by_id[gpu::G_DeleteFramebuffers],
           gpu::by_id[gpu::G_SwapWindow], gpu::by_id[gpu::G_Clear]);
    printf("%ld checks (%llu OpenGL calls compared), %ld differ, %ld protocol failures, %ld kept original, %ld footprint "
           "misses, %zu functions never checked\n",
           g_checks_total, g_gl_calls, g_mismatch_total, g_proto_total, g_kept_total, g_fp_total, unchecked);
    return g_mismatch_total || g_fp_total || g_proto_total || unchecked ? 1 : 0;
}
}  // namespace hx

int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    SetUnhandledExceptionFilter(hx::unhandled);
    if (!getenv("VP_WORLD_CHILD")) return hx::relaunch();
    return hx::main2(argc, argv);
}
