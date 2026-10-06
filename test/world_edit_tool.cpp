// world_edit_tool.cpp -- M3 UI stage, step U5 (group A): every rewrite of hook/edit_view.cpp, hook/edit_tool.cpp and
// hook/edit_leftover.cpp (library `edit`: edit.obj, modtool.obj, and the $E initialisers of all five edit objects) against its
// original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_edit_tool.cpp
//        /Fo<dir>\ /Fe<dir>\world_edit_tool.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86 /STACK:0x800000
//   run:   world_edit_tool.exe [rounds] [seed]          (VP_TRACE=1: one line per function; VP_ONLY=text: those only;
//          VP_DEBUG_WORLD=1: the world's objects and every fault's registers)
//
// Loads out\race_v10.exe at 0x400000 the way test/world_paintkit.cpp does (a child process with the range reserved) and
// includes the three rewrite files with PORT_FN redefined to list each function (its v1.0 address, the rewrite, its calling
// convention, stack arguments and return, its footprint). VP_FAITHFUL: the rewrites exactly as the originals.
//
// The world. Once: the $E static initialisers of libraries ui and edit (the ModelViewer and the TextureViewer at their
// statics, by $E62 / $E67), the game's UIBegin (the styles); a car list for the root's GetCarFileName / GetMaxCarFileNames
// (the originals run); then the model tool's state as ModTool leaves it, built by the originals: the two mrModelInfos
// (create_model_info), five ModBuilders (ModBuilderCreate) filled by group B's original ModBuilder* functions -- an empty
// one, a generated quad, a generated three-surface solid with smoothing flags, texture points and texture triangles, the
// stock "viper.car" model of the install (its smallest MINF of a size the builder takes, read-only from the resource set's
// file, through ModBuilderImportMI), and the undo builder; the infos copied from them (ModBuilderBuildInfo /
// BuildGeometry); a scratch info; the two views put in a window by the game's WidgetCreateWindow and _UIAddItems (their
// CustomWidgets), then their own Create. This is the pristine state.
//
// Then for EVERY function, `rounds` times (ten times that for one that doesn't run a dialog or a loop): pristine restored;
// half the rounds POISONED (.data/.bss random but for the strings and tables the code reads and the objects it follows);
// then the world's state random: the builder in use (one of the four), the texture points' coordinates (NaN / infinite /
// out of 0..1 now and then), the statics (mode -1..6, tool 0..4, lock 0..3, the surface, vertex, texture point and
// triangle picked -- in range and out --, the selection, the zoom, the scroll axes -- zero totals among them --, the
// slider, the colours, the file name, the transform and its flag, the modified flag), the views (the 3D view's angles,
// distance -- NaN / infinite / below 0.01 --, drag and mouse, its frame and the camera; the texture view's rectangle, its
// mouse coordinates, zoom, texture size, the capture overlays: counts, points, the one shown -1..11); arguments made for
// the function (the views, points around their rectangles and past them, keys, matrices and vectors with NaN, infinities,
// signalling NaNs and denormals, infos, names, destructor flags); for the dialogs (ModTool, EditMenu, the buttons' boxes and
// dialogs) a random input script ended by a watchdog. The stack filled with one pattern after the FPU is set; the
// ORIGINAL runs from a raw call thunk; its results kept, the snapshot restored, the REWRITE runs. Compared: .data/.bss/.idata
// and the arena (a pointer into this thread's stack in both counts as equal), the return value, st0, the x87 depth, bytes
// popped, ebx / esi / edi / ebp, faults, and every stub's call log. Every byte the original changed must lie in the
// rewrite's footprint (unless replay_only); a pure one changes nothing. The x87 at 24 or 53 bits (alternating rounds).
//
// Stubbed (logged; state in the arena so both passes see the same): memory, logs, sync, atexit, input and time (the input
// script, ScanDown by a random key mask), every 2D draw (stamps, fonts, palettes, clear, rect, line, point, XOR point,
// circle, triangle, paste-zoom), the canvases (gxAllocCanvas, gxFreeCanvas), the textures (gxGetTexture, gxForgetTexture,
// gxFreeAllTextures, gxGrabTexture -- a canvas, or a failure --, gxReleaseTexture, gxGetTextureSize), gxWriteBMP, the 3D
// renderer (mrSetView, mrSetProjection, mrModelSetClipDist, mrSetCamera, mrModelCreate / Destroy / Build -- logged with the
// info's counts and a hash of its arrays --, mrModelLoad / Unload / Draw, mrLightCalc, mrModelPick / PickVertex / PickEdge --
// answers from the round, in range and out --, mrModelGetExtents, mrModelInfoSave), Xlator::xlate, sprintf, atof, the file
// boxes (UIDoOpenFileBox / UIDoSaveFileBox: a name given back, or cancel), FileFindFirst / FileFindClose, the resource sets,
// and group B's file readers and writer: ModBuilderImportDXF / ImportMOD / Import3DS (a failure, or the builder cleared and
// filled with a generated model by the original ModBuilder* functions) and Write3DS. The game's own code everywhere else:
// the widget toolkit (UIDoDialog, _UIAddItems, the boxes, WidgetExit, the notifications), UIDialogItem's constructor,
// UICustomControl's, the matrix and vector functions, group B's ModBuilder* (all but the file readers), strrchr,
// GetCarFileName / GetMaxCarFileNames, and every function of this group (each rewrite is checked against its original with
// the same callees).
#define _CRT_SECURE_NO_WARNINGS
#if defined(__GNUC__) && !defined(__clang__)
#define _WIN32_WINNT 0x0A00               // (mingw: GetCurrentThreadStackLimits)
#endif
#include <windows.h>
#include <intrin.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif

#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#include "../hook/port.h"

// ---- the registry: PORT_FN lists each function -------------------------------------------------------------------------
struct Ent {
    uint32_t v10;
    const char* name;
    void* fn;
    int fast;                                   // __fastcall (a __thiscall rewrite): the first two words are ecx, edx
    int nstack;                                 // stack argument dwords
    int ret;                                    // return width in bytes (0: void)
    int retf;                                   // returns a float in st0
    void (*fp)(Footprint&, const uint32_t*);    // the footprint, from the argument words (ecx, edx first if fast)
};
static Ent g_fns[512];
static int g_nfns;
struct EntReg { EntReg(const Ent& e) { if (g_nfns < 512) g_fns[g_nfns++] = e; } };
template <typename T> static constexpr int words_of() { return (int)((sizeof(T) + 3) / 4); }
template <typename T> static T from_words(const uint32_t* w) { T t; memcpy(&t, w, sizeof(T)); return t; }
template <typename R> static constexpr int ret_width() { if constexpr (std::is_void_v<R> || std::is_floating_point_v<R>) return 0; else return (int)sizeof(R); }
template <typename... A> struct Offs {
    static constexpr int n = sizeof...(A);
    static constexpr int total = (0 + ... + words_of<A>());
    template <size_t I> static constexpr int off() {
        constexpr int w[] = {words_of<A>()..., 0};
        int o = 0;
        for (size_t k = 0; k < I; k++) o += w[k];
        return o;
    }
};
template <typename F> struct Sig;
template <typename R, typename... A> struct Sig<R(__cdecl*)(A...)> {
    enum { fast = 0 };
    static constexpr int nstack = Offs<A...>::total;
    static constexpr int ret = ret_width<R>();
    static constexpr int retf = std::is_floating_point_v<R> ? 1 : 0;
    template <void (*FP)(Footprint&, A...), size_t... I> static void fpi(Footprint& f, const uint32_t* w, std::index_sequence<I...>) {
        FP(f, from_words<A>(w + Offs<A...>::template off<I>())...);
        (void)w;
    }
    template <void (*FP)(Footprint&, A...)> static void fp(Footprint& f, const uint32_t* w) { fpi<FP>(f, w, std::index_sequence_for<A...>()); }
};
template <typename R, typename... A> struct Sig<R(__fastcall*)(A...)> {
    enum { fast = 1 };
    static constexpr int nstack = Offs<A...>::total - 2;
    static constexpr int ret = ret_width<R>();
    static constexpr int retf = std::is_floating_point_v<R> ? 1 : 0;
    template <void (*FP)(Footprint&, A...), size_t... I> static void fpi(Footprint& f, const uint32_t* w, std::index_sequence<I...>) {
        FP(f, from_words<A>(w + Offs<A...>::template off<I>())...);
    }
    template <void (*FP)(Footprint&, A...)> static void fp(Footprint& f, const uint32_t* w) { fpi<FP>(f, w, std::index_sequence_for<A...>()); }
};
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                                 \
    static EntReg VP_CAT(reg_, NEW)({V10, NAME, (void*)&NEW, Sig<decltype(&NEW)>::fast, Sig<decltype(&NEW)>::nstack,   \
                                     Sig<decltype(&NEW)>::ret, Sig<decltype(&NEW)>::retf, &Sig<decltype(&NEW)>::fp<&FP>});

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { printf("footprint: object() not expected (%s)\n", what); add(obj, 4, what); }
void Footprint::stack_ptr(void* p, const char* what) { add(p, 4, what); }

#include "../hook/edit_view.cpp"
#include "../hook/edit_tool.cpp"
#include "../hook/edit_leftover.cpp"

using namespace edt;
using uit::Widget; using uit::WidgetWindow; using uit::CustomWidget; using uit::UIDialogItem;
using uit::S_GX_CANVAS; using uit::S_ACTIVE; using uit::S_EXIT; using uit::S_EXIT_CODE; using uit::S_SCREEN_W; using uit::S_SCREEN_H;
using uit::S_XLATOR_COOKIE; using uit::S_DIALOG_IDLE; using uit::S_UI_DT; using uit::S_CURSOR; using uit::S_MOUSE_X; using uit::S_MOUSE_Y;

// ---- random numbers --------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x3c6ef372u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int irange(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float range(float a, float b) { return a + (b - a) * uni(); }
static bool g_nans;                                     // VP_NANS=1: NaNs (signalling ones half the time) far more often
static uint32_t wild_bits() {
    if (g_nans && chance(70)) return (chance(50) ? 0x7f800001u : 0x7fc00000u) | (rnd() & 0x803fffff);
    switch (rnd() % 9) {
    case 0: return chance(50) ? 0x7fc00000u : 0xffc00000u;
    case 1: return chance(50) ? 0x7f800000u : 0xff800000u;
    case 2: return chance(50) ? 0u : 0x80000000u;
    case 3: return fbits(range(-1e30f, 1e30f));
    case 4: return rnd() & 0x807fffff;                            // denormals
    case 5: return (chance(50) ? 0x7f800001u : 0xff800001u) | (rnd() & 0x3fffff);   // signalling NaNs
    default: return fbits(range(-100.0f, 100.0f));
    }
}
static uint32_t fval_bits(float lo, float hi) { return chance(g_nans ? 45 : 12) ? wild_bits() : fbits(range(lo, hi)); }
static float fval(float lo, float hi) { return bitsf(fval_bits(lo, hi)); }

// ---- the original, loaded at 0x400000 ------------------------------------------------------------------------------------
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
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, TRUE, CREATE_SUSPENDED, 0, 0, &si, &pi)) { printf("can't start the test process\n"); return 2; }
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

// ---- memory: the writable sections and the arena ---------------------------------------------------------------------------
// The arena: the stubs' state, the world's objects, then the heap (MemAlloc). Only the part in use is saved, restored and
// compared: [0, g_hw), g_hw the heap's high-water mark (ModTool's two builders take 2 MB of it); above it the arena holds a
// fixed pattern, put back wherever a pass reached past it.
static uint8_t* const DATA = (uint8_t*)0x004e1000;      // .data and its .bss
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* const IDATA = (uint8_t*)0x005d7000;
enum { IDATA_BYTES = 0x136a };
enum { ARENA_BYTES = 0x800000 };
enum : uint32_t { A_HEAP = 0x100000 };
static uint8_t* g_arena;
static uint32_t g_hw = A_HEAP;                           // bytes of the arena in use (a multiple of 0x1000)
struct Mem { uint8_t* data; uint8_t* idata; uint8_t* arena; uint32_t n; };
static Mem g_pristine, g_snap, g_after;
static Mem mem_alloc() { return {(uint8_t*)malloc(DATA_BYTES), (uint8_t*)malloc(IDATA_BYTES), (uint8_t*)malloc(ARENA_BYTES), 0}; }
static void mem_save(Mem& m, uint32_t n) { memcpy(m.data, DATA, DATA_BYTES); memcpy(m.idata, IDATA, IDATA_BYTES); memcpy(m.arena, g_arena, n); m.n = n; }
static void mem_load(const Mem& m, uint32_t n) { memcpy(DATA, m.data, DATA_BYTES); memcpy(IDATA, m.idata, IDATA_BYTES); memcpy(g_arena, m.arena, n); }
static ULONG_PTR g_stack_lo, g_stack_hi;
static bool on_stack(uint32_t a) { return a >= g_stack_lo && a < g_stack_hi; }
static uint32_t next_diff(const uint8_t* a, const uint8_t* b, uint32_t n, uint32_t i) {
    while (i < n) {
        const uint32_t e = (i | 0xfff) + 1 < n ? (i | 0xfff) + 1 : n;
        if (!memcmp(a + i, b + i, e - i)) { i = e; continue; }
        while (i < e && a[i] == b[i]) i++;
        if (i < e) return i;
    }
    return n;
}
static uint32_t block_diff(const uint8_t* saved, const uint8_t* live, uint32_t n, uint32_t i) {
    while (i < n) {
        i = next_diff(saved, live, n, i);
        if (i >= n) return n;
        bool ok = false;
        for (uint32_t s = i >= 3 ? i - 3 : 0; s <= i && s + 4 <= n; s++) {
            uint32_t a, b;
            memcpy(&a, saved + s, 4);
            memcpy(&b, live + s, 4);
            if (on_stack(a) && on_stack(b)) { ok = true; i = s + 4; break; }
        }
        if (!ok) return i;
    }
    return n;
}
static uint32_t mem_diff(const Mem& m, uint32_t n) {
    uint32_t i = block_diff(m.data, DATA, DATA_BYTES, 0);
    if (i < DATA_BYTES) return 0x004e1000 + i;
    i = block_diff(m.idata, IDATA, IDATA_BYTES, 0);
    if (i < IDATA_BYTES) return 0x005d7000 + i;
    i = block_diff(m.arena, g_arena, n, 0);
    if (i < n) return (uint32_t)(uintptr_t)(g_arena + i);
    return 0;
}

// ---- the stubs' state (in the arena, so each pass starts from the same) ------------------------------------------------------
enum { OP_NOP, OP_MOVE_W, OP_MOVE_XY, OP_DOWN, OP_UP, OP_RDOWN, OP_RUP, OP_KEY };
struct HState {
    uint32_t heap_next, heap_max;
    int32_t time, frames, frame_limit;
    int32_t script_n, script_pos;
    int32_t script[64][3];
    int32_t mouse_x, mouse_y;
    int32_t pal_next, alloc_canvas_next;
    uint32_t scan;                              // ScanDown: bit k for key 0xfc + k
    uint32_t seed;                              // the stubs' answers (picks, textures, extents, files)
    int32_t grab_ok, tex_size, save_ok, write_ok, find_ok, import_ok, box_ok;
    int32_t model_next, tex_next, picks;
};
#define HS ((HState*)g_arena)

// ---- the log ----------------------------------------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 17 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
enum { NCOV = 13 };
static long g_cov[NCOV];                                 // (VP_COVER: the coverage probes' counts)
static void L(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static uintptr_t g_self_lo, g_self_hi;
static bool readable(const void* p, uint32_t n) {
    const uint64_t a = (uint64_t)(uintptr_t)p, e = a + (uint64_t)n;
    if (a >= (uintptr_t)g_arena && e <= (uint64_t)(uintptr_t)g_arena + ARENA_BYTES) return true;
    if (a >= 0x401000 && e <= 0x5d9000) return true;
    if (a >= g_self_lo && e <= g_self_hi) return true;
    if (n && on_stack((uint32_t)a) && e - 1 <= 0xffffffffull && on_stack((uint32_t)(e - 1))) return true;
    return false;
}
static uint32_t P(const void* p) { return on_stack((uint32_t)(uintptr_t)p) ? 'STAK' : (uint32_t)(uintptr_t)p; }
static void LS(const char* s, int max = 0x400) {
    if (!s || !readable(s, 1)) { L('BADS'); L((uint32_t)(uintptr_t)s); return; }
    uint32_t w = 0;
    int i = 0;
    for (; i < max && readable(s + i, 1) && s[i]; i++) {
        w = w << 8 | (uint8_t)s[i];
        if ((i & 3) == 3) { L(w); w = 0; }
    }
    L(w);
    L((uint32_t)i);
}
static void LN(const char* s, int n) {
    if (n < 0 || n > 0x1000) { L('NBAD'); L((uint32_t)n); return; }
    if (!readable(s, (uint32_t)(n ? n : 1))) { L('BADS'); return; }
    uint32_t w = 0;
    for (int i = 0; i < n; i++) { w = w << 8 | (uint8_t)s[i]; if ((i & 3) == 3) { L(w); w = 0; } }
    L(w);
    L((uint32_t)n);
}
static uint32_t hash_bytes(const void* p, uint32_t n) {
    uint32_t h = 2166136261u;
    if (!readable(p, n ? n : 1)) return 0xbadb10cu;
    for (uint32_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static void L_canvas() {
    const gxCanvas* c = UI_GP(gxCanvas, S_GX_CANVAS);
    L(P(c));
    if (c && readable(c, sizeof(gxCanvas))) { L((uint32_t)c->cx0); L((uint32_t)c->cy0); L((uint32_t)c->cx1); L((uint32_t)c->cy1); }
}
static void L_block(const void* p, int dwords) {
    if (!readable(p, 4u * (uint32_t)dwords)) { L('BADB'); L(P(p)); return; }
    const uint32_t* w = (const uint32_t*)p;
    for (int i = 0; i < dwords; i++) L(w[i]);
}
static uint32_t hash_str(const char* s) {
    uint32_t h = 2166136261u;
    if (!s || !readable(s, 1)) return 0x5eed;
    for (; readable(s, 1) && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static uint32_t ans(uint32_t salt) { return (HS->seed ^ (salt * 2654435761u)) * 0x9e3779b1u ^ (uint32_t)HS->picks++ * 40503u; }

// ---- stubs: memory, logs, sync ---------------------------------------------------------------------------------------------------
static void* __cdecl stub_MemAlloc(int n) {
    L('ALOC'); L((uint32_t)n);
    uint32_t a = (HS->heap_next + 7) & ~7u;
    if (a < A_HEAP || n < 0 || n > 0x80000 || a + (uint32_t)n > ARENA_BYTES - 0x100) { L('OOM '); return 0; }
    HS->heap_next = a + (uint32_t)n;
    if (HS->heap_next > HS->heap_max) HS->heap_max = HS->heap_next;
    return g_arena + a;
}
static void __cdecl stub_delete(void* p) { L('DEL '); L(P(p)); }
static void L_va(const char* fmt, const uint32_t* w) {
    L(P(fmt));
    if (!fmt || !readable(fmt, 1)) return;
    int nw = 0;
    for (const char* p = fmt; readable(p, 1) && *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.*lh", *p)) { if (*p == '*') nw++; p++; }
        if (!*p) break;
        if (*p == 's') { const char* s = (const char*)(uintptr_t)w[nw++]; LS(s); continue; }
        nw += strchr("eEfgG", *p) ? 2 : 1;
    }
    for (int i = 0; i < nw && i < 16; i++) L(on_stack(w[i]) ? 'STAK' : w[i]);
}
static void __cdecl stub_LogReport(const char* fmt, ...) { L('LOGR'); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); }
static void __cdecl stub_LogPanic(const char* fmt, ...) { L('PANC'); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); }
static int __cdecl stub_SingleBegin(const char* s) { L('SBEG'); LS(s); return 0x5151; }
static void __cdecl stub_SingleEnd(int h, const char*, int) { L('SEND'); L((uint32_t)h); }
static int __cdecl stub_atexit(uint32_t fn) { L('ATEX'); L(fn); return 0; }
#if defined(__GNUC__) && !defined(__clang__)
// (mingw has no _ReturnAddress / _AddressOfReturnAddress: the builtins; the frame address forces ebp, the return
// address sits just above it)
static void __cdecl stub_crt_fatal(int code) {
    printf("the game's CRT reached a fatal path (%d, from %p): ending the test\n", code, __builtin_return_address(0));
    if (getenv("VP_DEBUG_WORLD")) {
        const uint32_t* sp = (const uint32_t*)__builtin_frame_address(0) + 1;
        for (int i = 0; i < 256; i++)
            if (sp[i] >= 0x401000 && sp[i] < 0x4e0000) printf("  [+%x] %08x\n", 4 * i, sp[i]);
    }
    fflush(stdout);
    ExitProcess(3);
}
#else
static void __cdecl stub_crt_fatal(int code) {
    printf("the game's CRT reached a fatal path (%d, from %p): ending the test\n", code, _ReturnAddress());
    if (getenv("VP_DEBUG_WORLD")) {
        const uint32_t* sp = (const uint32_t*)_AddressOfReturnAddress();
        for (int i = 0; i < 256; i++)
            if (sp[i] >= 0x401000 && sp[i] < 0x4e0000) printf("  [+%x] %08x\n", 4 * i, sp[i]);
    }
    fflush(stdout);
    ExitProcess(3);
}
#endif
static int __cdecl stub_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}

// ---- stubs: input and time -----------------------------------------------------------------------------------------------------
static int32_t __cdecl stub_PTimeNow() {
    L('TIME');
    HS->time += 16;
    HS->frames++;
    if (HS->frames > HS->frame_limit) { UI_G8(S_EXIT) = 1; UI_G32(S_EXIT_CODE) = 0x77; L('WDOG'); }
    return HS->time;
}
struct MouseEv { int32_t type, x, y, state; };
static bool widget_center(int k, int32_t* x, int32_t* y) {
    const WidgetWindow* w = UI_GP(WidgetWindow, S_ACTIVE);
    if (!w || !readable(w, sizeof(WidgetWindow)) || w->count <= 0 || w->count > 256) return false;
    const Widget* g = w->widgets[(uint32_t)k % (uint32_t)w->count];
    if (!g || !readable(g, sizeof(Widget))) return false;
    *x = w->x + (g->x0 + g->x1) / 2 + (k & 7) - 3;
    *y = w->y + (g->y0 + g->y1) / 2 + ((k >> 3) & 7) - 3;
    return true;
}
static uint8_t __cdecl stub_MouseGetEvent(MouseEv* ev) {
    L('MGEV');
    while (HS->script_pos < HS->script_n) {
        const int32_t* op = HS->script[HS->script_pos];
        if (op[0] == OP_KEY) return 0;
        HS->script_pos++;
        int32_t t = -1;
        switch (op[0]) {
        case OP_NOP: return 0;
        case OP_MOVE_W: if (!widget_center(op[1], &HS->mouse_x, &HS->mouse_y)) { HS->mouse_x = op[2]; HS->mouse_y = op[1]; } t = 6; break;
        case OP_MOVE_XY: HS->mouse_x = op[1]; HS->mouse_y = op[2]; t = 6; break;
        case OP_DOWN: t = 0; break;
        case OP_UP: t = 1; break;
        case OP_RDOWN: t = 2; break;
        case OP_RUP: t = 3; break;
        }
        ev->type = t; ev->x = HS->mouse_x; ev->y = HS->mouse_y; ev->state = 0;
        L((uint32_t)t); L((uint32_t)ev->x); L((uint32_t)ev->y);
        return 1;
    }
    return 0;
}
static uint8_t __cdecl stub_KeyHit() {
    L('KHIT');
    return HS->script_pos < HS->script_n && HS->script[HS->script_pos][0] == OP_KEY ? 1 : 0;
}
static uint16_t __cdecl stub_KeyGet() {
    L('KGET');
    if (HS->script_pos < HS->script_n && HS->script[HS->script_pos][0] == OP_KEY) {
        const uint16_t k = (uint16_t)HS->script[HS->script_pos][1];
        HS->script_pos++;
        L(k);
        return k;
    }
    return 0;
}
static void __cdecl stub_MousePeek(int32_t* x, int32_t* y, int32_t* st) { L('MPEK'); *x = HS->mouse_x; *y = HS->mouse_y; *st = 0; }
static void __cdecl stub_MouseClear() { L('MCLR'); }
static void __cdecl stub_KeyClear() { L('KCLR'); }
static uint8_t __cdecl stub_ScanDown(uint32_t k) { L('SCAN'); L(k & 0xff); return (uint8_t)((HS->scan >> ((k - 0xfc) & 7)) & 1); }

// ---- stubs: the 2D calls --------------------------------------------------------------------------------------------------------
struct Fake { uint32_t magic; char name[32]; int32_t w, h, count, hx, hy, asc, desc, cw; };
static Fake g_fakes[96];
static int g_nfakes;
static Fake* fake_for(const char* name, uint32_t magic) {
    char nm[32] = "?";
    if (name && readable(name, 1)) { strncpy(nm, name, 31); nm[31] = 0; }
    for (int i = 0; i < g_nfakes; i++) if (g_fakes[i].magic == magic && !strcmp(g_fakes[i].name, nm)) return &g_fakes[i];
    if (g_nfakes >= 96) return &g_fakes[0];
    Fake* f = &g_fakes[g_nfakes++];
    const uint32_t h = hash_str(nm) ^ magic;
    f->magic = magic;
    strcpy(f->name, nm);
    f->w = 12 + (int)(h % 40); f->h = 10 + (int)((h >> 8) % 30); f->count = 1 + (int)((h >> 16) % 6);
    f->hx = (int)((h >> 20) % 5); f->hy = (int)((h >> 24) % 5);
    f->asc = 7 + (int)(h % 4); f->desc = 2 + (int)((h >> 4) % 3); f->cw = 5 + (int)((h >> 12) % 4);
    return f;
}
static const Fake* as_fake(const void* p) {
    for (int i = 0; i < g_nfakes; i++) if (p == &g_fakes[i]) return &g_fakes[i];
    return 0;
}
static int32_t fake_val(const void* p, int which) {
    const Fake* f = as_fake(p);
    if (f) {
        const int32_t v[] = {f->w, f->h, f->count, f->hx, f->hy, f->asc, f->desc, f->cw};
        return v[which];
    }
    const uint32_t h = (uint32_t)(uintptr_t)p * 2654435761u;
    const int32_t v[] = {(int32_t)(8 + h % 20), (int32_t)(6 + (h >> 8) % 20), (int32_t)(1 + (h >> 16) % 4), 1, 2, 8, 2, 6};
    return v[which];
}
static void* __cdecl stub_gxGetStamp(const char* name) { L('GSTP'); LS(name); return fake_for(name, 'STMP'); }
static void __cdecl stub_gxForgetStamp(void* s) { L('FSTP'); L(P(s)); }
static int32_t __cdecl stub_gxStampWidth(const void* s) { L('STW '); L(P(s)); return fake_val(s, 0); }
static int32_t __cdecl stub_gxStampHeight(const void* s) { L('STH '); L(P(s)); return fake_val(s, 1); }
static int32_t __cdecl stub_gxStampCount(const void* s) { L('STC '); L(P(s)); return fake_val(s, 2); }
static void __cdecl stub_gxStampHotSpot(const void* s, int32_t* x, int32_t* y) { L('STHS'); L(P(s)); *x = fake_val(s, 3); *y = fake_val(s, 4); }
static uint8_t __cdecl stub_gxStampHitTest(const void* s, int32_t x, int32_t y) { L('STHT'); L(P(s)); L((uint32_t)x); L((uint32_t)y); return (uint8_t)((x ^ y) & 1); }
static void __cdecl stub_gxDrawStamp(const void* s, int32_t x, int32_t y, int32_t frame, const void* pal) {
    L('DSTP'); L(P(s)); L((uint32_t)x); L((uint32_t)y); L((uint32_t)frame); L(P(pal)); L_canvas();
}
static void* __cdecl stub_gxFontGet(const char* name) { L('FGET'); LS(name); return fake_for(name, 'FONT'); }
static void __cdecl stub_gxFontForget(void* f) { L('FFGT'); L(P(f)); }
static int32_t __cdecl stub_gxFontHeight(const void* f) { L('FHT '); L(P(f)); return fake_val(f, 5) + fake_val(f, 6); }
static int32_t __cdecl stub_gxFontAscent(const void* f) { L('FASC'); L(P(f)); return fake_val(f, 5); }
static int32_t __cdecl stub_gxFontDescent(const void* f) { L('FDSC'); L(P(f)); return fake_val(f, 6); }
static int32_t __cdecl stub_gxFontStringWidth(const void* f, const char* s) {
    L('FSW '); L(P(f)); LS(s);
    return readable(s, 1) ? (int32_t)strlen(s) * fake_val(f, 7) : 0;
}
static int32_t __cdecl stub_gxFontStringWidthN(const void* f, const char* s, int32_t n) { L('FSWN'); L(P(f)); LN(s, n); return n * fake_val(f, 7); }
static void __cdecl stub_gxFontPrintN(const void* f, const void* pal, uint32_t flags, int32_t x, int32_t y, const char* s, int32_t n) {
    L('FPRN'); L(P(f)); L(P(pal)); L(flags); L((uint32_t)x); L((uint32_t)y); LN(s, n); L_canvas();
}
static void __cdecl stub_gxFontPrintf(const void* f, const void* pal, uint32_t flags, int32_t x, int32_t y, const char* fmt, ...) {
    L('FPRF'); L(P(f)); L(P(pal)); L(flags); L((uint32_t)x); L((uint32_t)y); LS(fmt);
    va_list ap;
    va_start(ap, fmt);
    L_va(fmt, (const uint32_t*)ap);
    va_end(ap);
    L_canvas();
}
static void* __cdecl stub_gxPaletteCreate() { L('PCRE'); return (void*)(uintptr_t)(0x7a000000u + (uint32_t)(HS->pal_next++) * 16); }
static void __cdecl stub_gxPaletteDestroy(void* p) { L('PDES'); L(P(p)); }
static void __cdecl stub_gxPaletteMakeGradient(void* p, uint32_t a, uint32_t b) { L('PGRD'); L(P(p)); L(a); L(b); }
static gxCanvas* __cdecl stub_gxSetCanvas(gxCanvas* c) {
    L('SCNV'); L(P(c));
    gxCanvas* old = UI_GP(gxCanvas, S_GX_CANVAS);
    UI_GP(gxCanvas, S_GX_CANVAS) = c;
    return old;
}
static void __cdecl stub_gxClear(uint32_t c) { L('CLR '); L(c); L_canvas(); }
static void __cdecl stub_gxRect(int32_t a, int32_t b, int32_t c, int32_t d, uint32_t col) { L('RECT'); L(a); L(b); L(c); L(d); L(col); L_canvas(); }
static void __cdecl stub_gxLine(int32_t a, int32_t b, int32_t c, int32_t d, uint32_t col) { L('LINE'); L(a); L(b); L(c); L(d); L(col); L_canvas(); }
static void __cdecl stub_gxPoint(int32_t x, int32_t y, uint32_t col) { L('PNT '); L(x); L(y); L(col); L_canvas(); }
static void __cdecl stub_gxXORPoint(int32_t x, int32_t y) { L('XPNT'); L(x); L(y); L_canvas(); }
static void __cdecl stub_gxCircle(int32_t x, int32_t y, int32_t r, uint32_t col) { L('CIRC'); L(x); L(y); L(r); L(col); L_canvas(); }
static void __cdecl stub_gxTriangle(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f, uint32_t col) {
    L('TRI '); L(a); L(b); L(c); L(d); L(e); L(f); L(col); L_canvas();
}
static void __cdecl stub_gxPasteZoom(const gxCanvas* src, int32_t x, int32_t y, uint32_t fx, uint32_t fy) {
    L('PASZ'); L(P(src)); L((uint32_t)x); L((uint32_t)y); L(fx); L(fy); L_canvas();
}
static void __cdecl stub_gxPaste(const gxCanvas* src, int32_t x, int32_t y) { L('PAST'); L(P(src)); L((uint32_t)x); L((uint32_t)y); L_canvas(); }
static void __cdecl stub_gxPasteAlpha(const gxCanvas* src, int32_t x, int32_t y) { L('PASA'); L(P(src)); L((uint32_t)x); L((uint32_t)y); L_canvas(); }
static void __cdecl stub_gxClearNoAlpha(uint32_t c) { L('CLRN'); L(c); L_canvas(); }
static void __cdecl stub_gxXORLine(int32_t a, int32_t b, int32_t c, int32_t d) { L('XLIN'); L(a); L(b); L(c); L(d); L_canvas(); }
static void __cdecl stub_gxText(int32_t x, int32_t y, const char* s, uint32_t col) { L('TEXT'); L((uint32_t)x); L((uint32_t)y); LS(s); L(col); L_canvas(); }
static int32_t __cdecl stub_gxTextHeight() { L('TXTH'); return 8; }
static void __cdecl stub_gxAllocCanvas3(gxCanvas* c, int32_t w, int32_t h) {
    L('ACNV'); L(P(c)); L((uint32_t)w); L((uint32_t)h);
    c->format = 4; c->flags = 0; c->pixels = (uint8_t*)(uintptr_t)(0x7c000000u + (uint32_t)(HS->alloc_canvas_next++) * 0x100000u);
    c->w = w; c->h = h; c->pitch = w * 2; c->cx0 = 0; c->cy0 = 0; c->cx1 = w; c->cy1 = h;
}
static void __cdecl stub_gxFreeCanvas(gxCanvas* c) { L('FCNV'); L(P(c)); }
static uint8_t __cdecl stub_gxGrabScreen(gxCanvas* c) {
    L('GRAB'); L(P(c));
    c->format = 4; c->flags = 0x80; c->pixels = (uint8_t*)(uintptr_t)0x7b000000u; c->w = 640; c->h = 480; c->pitch = 1280;
    c->cx0 = 0; c->cy0 = 0; c->cx1 = 640; c->cy1 = 480;
    return 1;
}
static void __cdecl stub_gxReleaseScreen() { L('RELS'); }
static void __cdecl stub_gxFlipScreen() { L('FLPS'); }
static void __cdecl stub_mrBeginFrame() { L('MRBF'); }
static void __cdecl stub_mrEndFrame() { L('MREF'); }
// the textures
static int32_t __cdecl stub_gxGetTexture(const char* name, int32_t f) { L('GTX '); LS(name); L((uint32_t)f); return 0x40 + (int32_t)(hash_str(name) & 0xff); }
static void __cdecl stub_gxForgetTexture(int32_t t) { L('FTX '); L((uint32_t)t); }
static void __cdecl stub_gxFreeAllTextures() { L('FATX'); }
static int32_t __cdecl stub_gxGetTextureSize(int32_t t) { L('TXSZ'); L((uint32_t)t); return HS->tex_size; }
static uint8_t __cdecl stub_gxGrabTexture(int32_t t, gxCanvas* c) {
    L('GTEX'); L((uint32_t)t); L(P(c));
    if (!HS->grab_ok) return 0;
    c->format = 4; c->flags = 0; c->pixels = (uint8_t*)(uintptr_t)0x7d000000u; c->w = 128; c->h = 128; c->pitch = 256;
    c->cx0 = 0; c->cy0 = 0; c->cx1 = 128; c->cy1 = 128;
    return 1;
}
static void __cdecl stub_gxReleaseTexture() { L('RTEX'); }
static void __cdecl stub_gxWriteBMP(const char* name, const gxCanvas* c) { L('WBMP'); LS(name); L(P(c)); }
// the 3D renderer
static void __cdecl stub_mrSetView(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t c) { L('MRSV'); L(x); L(y); L(w); L(h); L(c); }
static void __cdecl stub_mrSetProjection(uint32_t a, uint32_t b, uint32_t c) { L('MRSP'); L(a); L(b); L(c); }
static void __cdecl stub_mrModelSetClipDist(uint32_t a, uint32_t b) { L('MRCD'); L(a); L(b); }
static void __cdecl stub_mrSetCamera(const void* fr) { L('MRSC'); L_block(fr, 12); }
static void __cdecl stub_mrModelDraw(int32_t m, const void* fr) { L('MRDR'); L((uint32_t)m); L_block(fr, 12); }
static int32_t __cdecl stub_mrModelLoad(const char* name) { L('MRLD'); LS(name); return 0x300 + HS->model_next++; }
static void __cdecl stub_mrModelUnload(int32_t m) { L('MRUL'); L((uint32_t)m); }
static int32_t __cdecl stub_mrModelCreate(const char* name, int32_t n) { L('MRCR'); LS(name); L((uint32_t)n); return 0x500 + HS->model_next++; }
static void __cdecl stub_mrModelDestroy(int32_t m) { L('MRDE'); L((uint32_t)m); }
static void L_info(const EdInfo* i) {
    L(P(i));
    if (!readable(i, sizeof *i)) { L('BADI'); return; }
    L((uint32_t)i->nverts); L((uint32_t)i->nsurfs); L((uint32_t)i->ntris);
    auto n = [](int32_t c, uint32_t sz) { return c > 0 && c < 0x4000 ? (uint32_t)c * sz : 0u; };
    L(hash_bytes(i->verts, n(i->nverts, 32)));
    L(hash_bytes(i->surfs, n(i->nsurfs, 32)));
    L(hash_bytes(i->tris, n(i->ntris, 8)));
}
static void __cdecl stub_mrModelBuild(int32_t m, const EdInfo* info, int32_t flags) { L('MRBD'); L((uint32_t)m); L_info(info); L((uint32_t)flags); }
static void __cdecl stub_mrLightCalc(uint8_t on) { L('MRLC'); L(on); }
static int32_t __cdecl stub_mrModelPick(const EdInfo* info, const void* fr, int32_t x, int32_t y) {
    L('PICK'); L(P(info)); L_block(fr, 12); L((uint32_t)x); L((uint32_t)y);
    const int32_t n = readable(info, sizeof *info) ? info->ntris : 0;
    const uint32_t a = ans(1);
    return (a & 7) == 0 ? -1 : (a & 7) == 1 ? (int32_t)(a >> 8) % 1000 : n > 0 ? (int32_t)((a >> 8) % (uint32_t)n) : -1;
}
static uint8_t __cdecl stub_mrModelPickVertex(const EdInfo* info, const void* fr, int32_t x, int32_t y, int32_t* tri, int32_t* vtx) {
    g_cov[12]++;
    L('PKVX'); L(P(info)); L_block(fr, 12); L((uint32_t)x); L((uint32_t)y);
    const uint32_t a = ans(2);
    const int32_t nv = readable(info, sizeof *info) ? info->nverts : 0;
    if ((a & 3) == 0 || nv <= 0) return 0;
    *vtx = (int32_t)((a >> 4) % (uint32_t)(nv < 0x1800 ? nv : 0x1800));
    const int32_t nt = info->ntris;
    *tri = nt > 0 ? (int32_t)((a >> 16) % (uint32_t)nt) : -1;
    return 1;
}
static uint8_t __cdecl stub_mrModelPickEdge(const EdInfo* info, const void* fr, int32_t x, int32_t y, int32_t* tri, int32_t* edge) {
    g_cov[11]++;
    L('PKED'); L(P(info)); L_block(fr, 12); L((uint32_t)x); L((uint32_t)y);
    const uint32_t a = ans(3);
    if ((a & 3) == 0) return 0;
    const int32_t nt = readable(info, sizeof *info) ? info->ntris : 0;
    *tri = (a & 0x30) == 0 ? -1 : nt > 0 ? (int32_t)((a >> 8) % (uint32_t)nt) : 0;
    *edge = (int32_t)((a >> 20) % 4);                     // (3: the original's "Unknown edge")
    return 1;
}
static void __cdecl stub_mrModelGetExtents(int32_t m, float* a, float* b, float* c, float* d, float* e, float* f) {
    L('MREX'); L((uint32_t)m);
    float* o[6] = {a, b, c, d, e, f};
    for (int i = 0; i < 6; i++) {
        const uint32_t x = ans(4 + i);
        *(uint32_t*)o[i] = (x & 0xf) == 0 ? 0x7fc00000u : fbits((float)(int32_t)(x >> 8 & 0xffff) / 1000.0f - 30.0f);
    }
}
static uint8_t __cdecl stub_mrModelInfoSave(const EdInfo* info, const char* name) { L('MISV'); L_info(info); LS(name); return (uint8_t)HS->save_ok; }
// text
static const char* const k_xl_text[8] = {"Ok", "Cancel", "Yes", "No", "Back", "Open", "Save", "A longer translation of a box's text"};
static void __fastcall stub_xlate(uint32_t* xl, int) {
    L('XLAT'); L(P(xl));
    xl[1] = (uint32_t)(uintptr_t)k_xl_text[hash_str((const char*)(uintptr_t)xl[0]) % 8];
    xl[2] = UI_GU32(S_XLATOR_COOKIE);
}
static const char* __cdecl stub_Xlate(const char* s) { L('XLTE'); LS(s); return k_xl_text[hash_str(s) % 8]; }
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    L('SPRF'); L(P(buf)); LS(fmt);
    va_list ap;
    va_start(ap, fmt);
    const uint32_t* w = (const uint32_t*)ap;
    int nw = 0;
    for (const char* p = fmt; readable(p, 1) && *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.*lh", *p)) { if (*p == '*') nw++; p++; }
        if (!*p) break;
        if (*p == 's') { LS((const char*)(uintptr_t)w[nw]); }
        nw += strchr("eEfgG", *p) ? 2 : 1;
    }
    for (int i = 0; i < nw && i < 16; i++) L(on_stack(w[i]) ? 'STAK' : w[i]);
    const int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    LS(buf);
    return r;
}
static double __cdecl stub_atof(const char* s) { L('ATOF'); LS(s); return readable(s, 1) ? atof(s) : 0.0; }
static void __cdecl stub_LocaleConvertNumeric(char* s) { L('LCNV'); LS(s); }
static int __cdecl stub_tolower(int c) { L('TLOW'); L((uint32_t)c); return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static uint8_t __cdecl stub_ClipboardGetText(char* buf, int n) { L('CLIP'); if (n > 0) buf[0] = 0; return 0; }
// files
static void* __cdecl stub_FileFindFirst(const char* pat, char* out, int n) {
    L('FFF '); LS(pat); L((uint32_t)n);
    if (!HS->find_ok) return (void*)(intptr_t)-1;
    if (n > 0 && readable(out, (uint32_t)n)) out[0] = 0;
    return (void*)(uintptr_t)0x4444;
}
static uint8_t __cdecl stub_FileFindNext(void* h, char*, int) { L('FFN '); L((uint32_t)(uintptr_t)h); return 0; }
static void __cdecl stub_FileFindClose(void* h) { L('FFC '); L((uint32_t)(uintptr_t)h); }
static uint8_t __cdecl stub_file_box(const char* title, const char* type, const char* pat, char* buf, int n, const char* dir) {
    L('FBOX'); LS(title); LS(type); LS(pat); LS(buf); L((uint32_t)n); LS(dir);
    const uint32_t a = ans(20);
    if ((a & 3) == 0 || !HS->box_ok) return 0;
    static const char* const names[] = {"C:\\models\\viper.mod", "tex2.tex", "noext", "a.b.dxf", "C:\\out\\export.3ds", "x"};
    const char* nm = names[(a >> 4) % 6];
    if (n > 0 && readable(buf, (uint32_t)n)) { strncpy(buf, nm, (size_t)n - 1); buf[n - 1] = 0; }
    LS(buf);
    return 1;
}
static void __cdecl stub_ResourceSetMustLoad(const char* s) { L('RSML'); LS(s); }
static void __cdecl stub_ResourceSetUnload(const char* s) { L('RSUL'); LS(s); }

// ---- group B's files: the readers fill the builder with a generated model (by the original ModBuilder* functions) ------------
typedef void(__cdecl* BClear_t)(void*);
typedef int(__cdecl* BAddSurface_t)(void*);
typedef void(__cdecl* BSetTex_t)(void*, int, const char*);
typedef void(__cdecl* BSetMat_t)(void*, int, uint32_t, uint32_t);
typedef void(__cdecl* BSetGroup_t)(void*, int, uint32_t);
typedef int(__cdecl* BAddVertex_t)(void*, const float*);
typedef int(__cdecl* BAddTri_t)(void*, const EdTri*);
typedef int(__cdecl* BAddTp_t)(void*, int, int, uint32_t, uint32_t);
typedef int(__cdecl* BAddTtri_t)(void*, int, int, int, int, int);
#define B_FN(T, a) ((T)(uintptr_t)(a))
// a generated model: `ns` surfaces over a ring of `nv` vertices (two rings of nv / 2), triangles between them (smoothing
// flags from the seed), texture points on the vertices and texture triangles from them
static void gen_model(void* b, uint32_t seed, int ns, int nv) {
    uint32_t r = seed | 1;
    auto rr = [&]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r; };
    B_FN(BClear_t, 0x004b71d0)(b);
    const int half = nv / 2 > 1 ? nv / 2 : 2;
    int vid[64];
    for (int i = 0; i < 2 * half && i < 64; i++) {
        const float a = 6.2831853f * (float)(i % half) / (float)half;
        float p[3] = {cosf(a) * (1.0f + 0.1f * (float)(rr() % 5)), (i < half ? -0.5f : 0.5f) * (float)(1 + rr() % 3), sinf(a)};
        vid[i] = B_FN(BAddVertex_t, 0x004b7370)(b, p);
    }
    int ntri = 0;
    EdTri tris[64];
    for (int i = 0; i < half && ntri < 62; i++) {
        const int a0 = vid[i], a1 = vid[(i + 1) % half], b0 = vid[half + i], b1 = vid[half + (i + 1) % half];
        EdTri t1 = {}, t2 = {};
        t1.v[0] = a0; t1.v[1] = a1; t1.v[2] = b0;
        t2.v[0] = a1; t2.v[1] = b1; t2.v[2] = b0;
        for (int k = 0; k < 3; k++) { t1.edge[k] = (uint8_t)(rr() & 1); t2.edge[k] = (uint8_t)(rr() & 1); }
        B_FN(BAddTri_t, 0x004b7470)(b, &t1);
        B_FN(BAddTri_t, 0x004b7470)(b, &t2);
        tris[ntri++] = t1;
        tris[ntri++] = t2;
    }
    for (int s = 0; s < ns; s++) {
        const int si = B_FN(BAddSurface_t, 0x004b7800)(b);
        char name[16];
        sprintf(name, "tex%d.tex", s);
        B_FN(BSetTex_t, 0x004b78e0)(b, si, name);
        B_FN(BSetMat_t, 0x004b7960)(b, si, rr() % 3, rr() % 2);
        B_FN(BSetGroup_t, 0x004b79c0)(b, si, rr() % 16);
        int tp[64];
        int ntp = 0;
        for (int i = 0; i < 2 * half && i < 64; i++) {
            const uint32_t u = fbits((float)(i % half) / (float)half), v = fbits(i < half ? 0.1f : 0.9f + 0.01f * (float)s);
            tp[ntp++] = B_FN(BAddTp_t, 0x004b7ba0)(b, si, vid[i], u, v);
        }
        for (int t = s; t < ntri; t += ns > 0 ? ns : 1) {
            int k[3];
            for (int j = 0; j < 3; j++) {
                k[j] = 0;
                for (int i = 0; i < 2 * half && i < 64; i++) if (vid[i] == tris[t].v[j]) k[j] = tp[i];
            }
            B_FN(BAddTtri_t, 0x004b7a00)(b, si, t, k[0], k[1], k[2]);
        }
    }
}
static void* g_import_target;                           // (none: the stubs fill the builder they're given)
static uint8_t __cdecl stub_ImportDXF(void* b, const char* name, uint8_t f) {
    L('IDXF'); L(P(b)); LS(name); L(f);
    if (!HS->import_ok) return 0;
    gen_model(b, hash_str(name) ^ 0xd8f, 1 + (int)(hash_str(name) % 3), 8 + 2 * (int)(hash_str(name) % 5));
    return 1;
}
static uint8_t __cdecl stub_ImportMOD(void* b, const char* name) {
    L('IMOD'); L(P(b)); LS(name);
    if (!HS->import_ok) return 0;
    gen_model(b, hash_str(name) ^ 0x30d, 1 + (int)(hash_str(name) % 4), 6 + 2 * (int)(hash_str(name) % 7));
    return 1;
}
static uint8_t __cdecl stub_Import3DS(void* b, const char* name) {
    L('I3DS'); L(P(b)); LS(name);
    if (!HS->import_ok) return 0;
    gen_model(b, hash_str(name) ^ 0x3d5, 2, 12);
    return 1;
}
static uint8_t __cdecl stub_Write3DS(const EdInfo* info, const char* name) { L('W3DS'); L_info(info); LS(name); return (uint8_t)HS->write_ok; }

// ---- coverage (VP_COVER=1): group B's edits that only a function's deeper paths make, counted per function -------------------
static const char* const k_cov_names[] = {"AddTextureTriangle", "MoveTexturePoint", "SetTriangle", "FacetAll", "SmoothAll",
                                          "DeleteTextureTriangle", "DeleteTexturePoint", "TranslateModel", "ScaleModel",
                                          "SetSurfaceGroup", "AddTexturePoint", "PickEdge", "PickVertex"};
static void cov(int k) { g_cov[k]++; L('COV0' + (uint32_t)k); }
static int __cdecl cov_AddTextureTriangle(void*, int, int, int, int, int) { cov(0); return 0; }
static void __cdecl cov_MoveTexturePoint(void*, int, int, uint32_t u, uint32_t v) { cov(1); L(u); L(v); }
static void __cdecl cov_SetTriangle(void*, int, const void* t) { cov(2); L_block(t, 4); }
static void __cdecl cov_FacetAll(void*) { cov(3); }
static void __cdecl cov_SmoothAll(void*) { cov(4); }
static void __cdecl cov_DeleteTextureTriangle(void*, int, int i) { cov(5); L((uint32_t)i); }
static uint8_t __cdecl cov_DeleteTexturePoint(void*, int, int i) { cov(6); L((uint32_t)i); return (uint8_t)(i & 1); }
static void __cdecl cov_TranslateModel(void*, const void* p) { cov(7); L_block(p, 3); }
static void __cdecl cov_ScaleModel(void*, const void* p) { cov(8); L_block(p, 3); }
static void __cdecl cov_SetSurfaceGroup(void*, int, int g) { cov(9); L((uint32_t)g); }
static int __cdecl cov_AddTexturePoint(void*, int, int vtx, uint32_t u, uint32_t v) { cov(10); L((uint32_t)vtx); L(u); L(v); return vtx & 7; }

// ---- the raw call ------------------------------------------------------------------------------------------------------------------
static uint32_t g_c_fn, g_c_ecx, g_c_edx, g_c_n, g_c_args[64];
static uint32_t g_r_eax, g_r_edx, g_r_ebx, g_r_esi, g_r_edi, g_r_ebp, g_r_esp, g_c_top, g_save_esp;
static uint16_t g_r_sw;
static double g_r_st0;
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((naked)) static void raw_call() {
    __asm__ volatile(
        "push ebx\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "push ebp\n\t"
        "mov dword ptr [%P[save_esp]], esp\n\t"
        "mov ecx, dword ptr [%P[c_n]]\n"
        "1:\n\t"
        "test ecx, ecx\n\t"
        "jz 2f\n\t"
        "dec ecx\n\t"
        "push dword ptr [%P[c_args] + ecx * 4]\n\t"
        "jmp 1b\n"
        "2:\n\t"
        "mov dword ptr [%P[c_top]], esp\n\t"
        "mov ebx, 0x0b0b0b0b\n\t"
        "mov esi, 0x05050505\n\t"
        "mov edi, 0x0d0d0d0d\n\t"
        "mov ebp, 0x0e0e0e0e\n\t"
        "mov ecx, dword ptr [%P[c_ecx]]\n\t"
        "mov edx, dword ptr [%P[c_edx]]\n\t"
        "call dword ptr [%P[c_fn]]\n\t"
        "mov dword ptr [%P[r_eax]], eax\n\t"
        "mov dword ptr [%P[r_edx]], edx\n\t"
        "mov dword ptr [%P[r_ebx]], ebx\n\t"
        "mov dword ptr [%P[r_esi]], esi\n\t"
        "mov dword ptr [%P[r_edi]], edi\n\t"
        "mov dword ptr [%P[r_ebp]], ebp\n\t"
        "mov dword ptr [%P[r_esp]], esp\n\t"
        "fnstsw word ptr [%P[r_sw]]\n\t"
        "fst qword ptr [%P[r_st0]]\n\t"
        "mov esp, dword ptr [%P[save_esp]]\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "ret"
        :: [save_esp] "i"(&g_save_esp), [c_n] "i"(&g_c_n), [c_args] "i"(g_c_args), [c_top] "i"(&g_c_top), [c_ecx] "i"(&g_c_ecx),
           [c_edx] "i"(&g_c_edx), [c_fn] "i"(&g_c_fn), [r_eax] "i"(&g_r_eax), [r_edx] "i"(&g_r_edx), [r_ebx] "i"(&g_r_ebx),
           [r_esi] "i"(&g_r_esi), [r_edi] "i"(&g_r_edi), [r_ebp] "i"(&g_r_ebp), [r_esp] "i"(&g_r_esp), [r_sw] "i"(&g_r_sw),
           [r_st0] "i"(&g_r_st0));
}
#else
__declspec(naked) static void raw_call() {
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov g_save_esp, esp
        mov ecx, g_c_n
    pushloop:
        test ecx, ecx
        jz pushed
        dec ecx
        push dword ptr g_c_args[ecx * 4]
        jmp pushloop
    pushed:
        mov g_c_top, esp
        mov ebx, 0x0b0b0b0b
        mov esi, 0x05050505
        mov edi, 0x0d0d0d0d
        mov ebp, 0x0e0e0e0e
        mov ecx, g_c_ecx
        mov edx, g_c_edx
        call dword ptr g_c_fn
        mov g_r_eax, eax
        mov g_r_edx, edx
        mov g_r_ebx, ebx
        mov g_r_esi, esi
        mov g_r_edi, edi
        mov g_r_ebp, ebp
        mov g_r_esp, esp
        fnstsw g_r_sw
        fst g_r_st0
        mov esp, g_save_esp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}
#endif
struct Result { int fault; uint32_t code, eip, ret, pops, regs[4], top; double st0; };
static uint32_t g_fault_code, g_fault_eip;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_eip = (uint32_t)e->ContextRecord->Eip;
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    if (getenv("VP_DEBUG_WORLD"))
        printf("  fault at %08x: address %08x, eax %08x ecx %08x ebx %08x\n", (uint32_t)e->ContextRecord->Eip,
               (uint32_t)e->ExceptionRecord->ExceptionInformation[1], (uint32_t)e->ContextRecord->Eax, (uint32_t)e->ContextRecord->Ecx,
               (uint32_t)e->ContextRecord->Ebx);
    if (getenv("VP_DEBUG_STACK")) {
        const uint32_t* sp = (const uint32_t*)(uintptr_t)e->ContextRecord->Esp;
        for (int i = 0; i < 64; i++)
            if (sp[i] >= 0x401000 && sp[i] < 0x4e0000) printf("    [esp+%x] %08x\n", 4 * i, sp[i]);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
__declspec(noinline) static void fill_stack() {
    volatile uint32_t buf[0x60000 / 4];
    for (uint32_t i = 0; i < sizeof buf / 4; i++) buf[i] = 0xcdcdcdcdu;
}
static unsigned g_pc;
__declspec(noinline) static Result run(const Ent& f, bool rewrite, const uint32_t* words) {
    Result r = {};
    g_log.n = 0;
    g_c_fn = rewrite ? (uint32_t)(uintptr_t)f.fn : f.v10;
    const uint32_t* stack = words;
    if (f.fast) { g_c_ecx = words[0]; g_c_edx = words[1]; stack = words + 2; }
    else { g_c_ecx = 0x00c0ffee; g_c_edx = 0x00d00d1e; }
    g_c_n = (uint32_t)f.nstack;
    for (int i = 0; i < f.nstack && i < 64; i++) g_c_args[i] = stack[i];
    unsigned cw;
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    fill_stack();
    if (vp_try([&] { raw_call(); }, fault_filter)) { r.fault = 1; r.code = g_fault_code; r.eip = g_fault_eip; }
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
#else
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    fill_stack();
    __try {
        raw_call();
    } __except (fault_filter(GetExceptionInformation())) { r.fault = 1; r.code = g_fault_code; r.eip = g_fault_eip; }
    __asm fninit
#endif
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    if (!r.fault) {
        const uint32_t mask = f.ret == 0 ? 0 : f.ret == 1 ? 0xffu : f.ret == 2 ? 0xffffu : 0xffffffffu;
        r.ret = g_r_eax & mask;
        r.pops = g_r_esp - g_c_top;
        r.regs[0] = g_r_ebx; r.regs[1] = g_r_esi; r.regs[2] = g_r_edi; r.regs[3] = g_r_ebp;
        r.top = (g_r_sw >> 11) & 7;
        r.st0 = f.retf ? g_r_st0 : 0;
    }
    return r;
}

// ---- the world -------------------------------------------------------------------------------------------------------------------
static uint32_t g_wv;
static void* wv(uint32_t n) { g_wv = (g_wv + 7) & ~7u; void* p = g_arena + g_wv; g_wv += n; memset(p, 0, n); if (g_wv > A_HEAP) { printf("the world outgrew its part of the arena\n"); ExitProcess(4); } return p; }
static char* wstr(const char* s) { char* p = (char*)wv((uint32_t)strlen(s) + 1); strcpy(p, s); return p; }
static uint32_t g_setup_ret;
static void call_orig(uint32_t fn, int fast, std::initializer_list<uint32_t> words) {
    Ent e = {fn, "(setup)", 0, fast, (int)words.size() - (fast ? 2 : 0), 4, 0, 0};
    uint32_t w[32];
    int i = 0;
    for (uint32_t x : words) w[i++] = x;
    g_pc = _PC_53;
    const Result r = run(e, false, w);
    g_setup_ret = r.ret;
    if (r.fault) printf("setup: %08x faulted (%08x at %08x)\n", fn, r.code, r.eip);
}
#define U(p) ((uint32_t)(uintptr_t)(p))

enum { NB = 4 };                                         // the builders in use: empty, quad, solid, stock
struct World {
    gxCanvas* screen;
    gxCanvas* canvas2;
    WidgetWindow* win;
    void* builders[NB];
    void* undo;
    const char* stock_name;                              // the stock model's (or "" if none could be read)
    EdInfo* info3;                                       // a scratch info (copy_model_info's, build_edge_model's)
    EdInfo* info_fake;                                   // destroy_model_info's (its arrays never really freed)
    uint8_t* scratch;                                    // fresh blocks (0x2000)
    float* mats;                                         // matrices / vectors for the maths (0x100 floats)
    char* texname;                                       // S_TEXNAME's (0x40)
    char* names[6];
    char* carlist;
    void* back;                                          // carback.stp
    int32_t ndrawn;
};
static World W;
static std::vector<uint32_t> g_setup_e;

static void read_inventory(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { printf("can't open %s\n", path); return; }
    char line[2048];
    fgets(line, sizeof line, f);
    while (fgets(line, sizeof line, f)) {
        char* c[6] = {line};
        int k = 1;
        for (char* p = line; *p && k < 6; p++)
            if (*p == ',') { *p = 0; c[k++] = p + 1; }
        if (k < 6) continue;
        const uint32_t va = (uint32_t)strtoul(c[0], 0, 16);
        if (va == 0x004b19f0 || va == 0x004b1a40) continue;      // $E63 / $E68: the views' destructors (atexit's), not run here
        if ((!strcmp(c[2], "ui") || !strcmp(c[2], "edit")) && c[5][0] == '$' && c[5][1] == 'E') g_setup_e.push_back(va);
    }
    fclose(f);
}
static UIDialogItem* it_(UIDialogItem* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h, const void* text,
                         int32_t i1c, const void* data, int32_t style) {
    memset(it, 0, sizeof *it);
    it->type = type; it->id = id; it->x = x; it->y = y; it->w = w; it->h = h; it->text = (const char*)text; it->i1c = i1c;
    it->data = (void*)data; it->style = style;
    return it + 1;
}
// the install's stock model: viper.car's smallest MINF with 16..0x180 triangles (a resource set: "0TSR", the count, the
// TOC of 36-byte entries, then each payload behind 8 bytes), read-only from the install, copied into the arena with its
// arrays' pointers made from its counts (as mrModelInfoGet does)
static EdInfo* load_stock_model(const char* install, char* name_out) {
    char path[MAX_PATH];
    sprintf(path, "%s\\viper.car", install);
    FILE* f = fopen(path, "rb");
    name_out[0] = 0;
    if (!f) return 0;
    std::vector<uint8_t> b;
    fseek(f, 0, SEEK_END);
    b.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(b.data(), 1, b.size(), f);
    fclose(f);
    if (b.size() < 16 || memcmp(b.data(), "0TSR", 4)) return 0;
    const uint32_t count = *(const uint32_t*)&b[4];
    size_t off = 16 + 36 * (size_t)count, best_off = 0, best_size = 0;
    int32_t best_t = 0x7fffffff;
    for (uint32_t i = 0; i < count && off + 8 <= b.size(); i++) {
        const uint8_t* e = &b[16 + 36 * i];
        const uint32_t type = *(const uint32_t*)(e + 16), size = *(const uint32_t*)(e + 24);
        if (type == 0x4d494e46 && off + 8 + 0x28 <= b.size() && !memcmp(&b[off + 4], "!IGM", 4)) {
            const int32_t* h = (const int32_t*)&b[off + 8];
            if (h[4] >= 16 && h[4] <= 0x180 && h[0] <= 0x300 && h[2] <= 12 && h[4] < best_t) {
                best_t = h[4]; best_off = off + 8; best_size = size;
                memcpy(name_out, e, 16);
                name_out[16] = 0;
            }
        }
        off += 8 + size;
    }
    if (!best_size) return 0;
    uint8_t* v = (uint8_t*)wv((uint32_t)best_size);
    memcpy(v, &b[best_off], best_size);
    EdInfo* info = (EdInfo*)v;
    uint8_t* p = v + 0x28;
    if (info->nverts > 0) { info->verts = p; p += 32u * (uint32_t)info->nverts; } else info->verts = 0;
    if (info->nsurfs > 0) { info->surfs = p; p += 32u * (uint32_t)info->nsurfs; } else info->surfs = 0;
    if (info->ntris > 0) { info->tris = p; p += 8u * (uint32_t)info->ntris; } else info->tris = 0;
    if (info->n18 > 0) { info->p1c = p; p += 16u * (uint32_t)info->n18; } else info->p1c = 0;
    info->p24 = info->n20 > 0 ? p : 0;
    return info;
}
static const char* k_cars[] = {"viper", "cobra", "gts", "rt10", "stingray", "zr1"};
static void build_world(const char* install) {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    HS->tex_size = 256;
    HS->grab_ok = 1;
    UI_G32(S_SCREEN_W) = 640;
    UI_G32(S_SCREEN_H) = 480;
    for (uint32_t a : g_setup_e) call_orig(a, 0, {});
    call_orig(0x004779a0, 0, {});                      // UIBegin
    W.screen = (gxCanvas*)wv(sizeof(gxCanvas));
    W.screen->format = 4; W.screen->pixels = (uint8_t*)0x7b000000; W.screen->w = 640; W.screen->h = 480; W.screen->pitch = 1280;
    W.screen->cx1 = 640; W.screen->cy1 = 480;
    W.canvas2 = (gxCanvas*)wv(sizeof(gxCanvas));
    memcpy((void*)W.canvas2, (const void*)W.screen, sizeof(gxCanvas));
    W.canvas2->w = 320; W.canvas2->h = 200; W.canvas2->cx0 = 10; W.canvas2->cy0 = 5; W.canvas2->cx1 = 300; W.canvas2->cy1 = 190;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    W.carlist = (char*)wv(32 * 6);
    for (int i = 0; i < 6; i++) strcpy(W.carlist + 32 * i, k_cars[i]);
    UI_GP(char, 0x00504340) = W.carlist;
    UI_G32(0x00504344) = 6;
    W.scratch = (uint8_t*)wv(0x2000);
    W.mats = (float*)wv(0x400);
    W.texname = (char*)wv(0x40);
    strcpy(W.texname, "tex0.tex");
    const char* nm[6] = {"C:\\models\\viper.mod", "x.dxf", "C:\\out\\thing.3ds", "noext", "a.b", "untitled.mod"};
    for (int i = 0; i < 6; i++) W.names[i] = wstr(nm[i]);
    // the model tool's state, as ModTool leaves it, by the originals
    call_orig(F_create_model_info, 0, {S_INFO});
    call_orig(F_create_model_info, 0, {S_INFO2});
    W.info3 = (EdInfo*)wv(sizeof(EdInfo));
    call_orig(F_create_model_info, 0, {U(W.info3)});
    W.info_fake = (EdInfo*)wv(sizeof(EdInfo));
    W.info_fake->verts = (uint8_t*)W.scratch; W.info_fake->surfs = W.scratch + 0x100; W.info_fake->tris = W.scratch + 0x200;
    for (int i = 0; i < NB; i++) {
        call_orig(F_ModBuilderCreate, 0, {0x200, 0x10});
        W.builders[i] = (void*)(uintptr_t)g_setup_ret;
    }
    call_orig(F_ModBuilderCreate, 0, {0x200, 0x10});
    W.undo = (void*)(uintptr_t)g_setup_ret;
    gen_model(W.builders[1], 0x1234, 1, 4);
    gen_model(W.builders[2], 0xbeef, 3, 24);
    {
        static char stock[20];
        EdInfo* mi = load_stock_model(install, stock);
        if (mi) {
            call_orig(0x004b95a0, 0, {U(W.builders[3]), U(mi)});                    // ModBuilderImportMI
            W.stock_name = stock;
        } else {
            gen_model(W.builders[3], 0x5eed, 4, 40);
            W.stock_name = "";
        }
    }
    UI_GP(void, S_BUILDER) = W.builders[2];
    UI_GP(void, S_UNDO) = W.undo;
    call_orig(F_copy_model_info, 0, {S_INFO, (call_orig(F_ModBuilderBuildInfo, 0, {U(W.builders[2])}), g_setup_ret)});
    call_orig(F_copy_model_info, 0, {S_INFO2, (call_orig(F_ModBuilderBuildGeometry, 0, {U(W.builders[2]), 0x004feb64}), g_setup_ret)});
    // the views: their statics' fields as ModTool sets them, put in a window (their CustomWidgets), then their Create
    W.back = stub_gxGetStamp("carback.stp");
    ModelViewer* mv = ed_mv();
    mv->model = 0x500; mv->point = 0x301; mv->capture = 0; mv->back = W.back;
    mv->frame = (EdFrame*)ED_P(S_FRAME); mv->camera = (EdFrame*)ED_P(S_CAMERA);
    call_orig(F_MatrixMakeIdentity, 0, {S_FRAME});
    call_orig(F_MatrixMakeIdentity, 0, {S_CAMERA});
    UIDialogItem* items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 4);
    UIDialogItem* it = items;
    it = it_(it, 23, 0, 0x14, 0x3c, 0x12c, 0x100, "", 0, ED_P(S_MV), 0);
    it = it_(it, 23, 0, 0x154, 0x3c, 0x100, 0x100, "", 0, ED_P(S_TV), 0);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    call_orig(uit::F_WidgetCreateWindow, 0, {0, 0, 0, 640, 480, 2});
    W.win = (WidgetWindow*)(uintptr_t)g_setup_ret;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    call_orig(uit::F_UIAddItems, 0, {U(W.win), U(items)});
    call_orig(0x004b4a70, 1, {S_MV, 0});                // ModelViewer::Create
    call_orig(0x004b5ab0, 1, {S_TV, 0});                // TextureViewer::Create
    if (getenv("VP_DEBUG_WORLD")) {
        printf("  builders %p %p %p %p, undo %p (stock model \"%s\"); window %p, %d widgets; heap at +%x\n", W.builders[0], W.builders[1],
               W.builders[2], W.builders[3], W.undo, W.stock_name, (void*)W.win, W.win->count, HS->heap_next);
    }
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
static int32_t coord(int32_t lo, int32_t hi) { return chance(90) ? irange(lo, hi) : (chance(50) ? irange(-100000, 100000) : (int32_t)rnd()); }
static int32_t b_count(const void* b, uint32_t off) { return *(const int32_t*)((const uint8_t*)b + off); }
// a builder's texture points' coordinates (in 0..1 mostly; out of it, huge or NaN now and then)
static void randomize_points(void* b) {
    const int32_t ns = b_count(b, 0x1c);
    uint8_t* surfs = *(uint8_t**)((uint8_t*)b + 0x18);
    for (int32_t s = 0; s < ns && s < 0x10; s++) {
        uint8_t* sf = surfs + 0x2c * s;
        const int32_t n = *(int32_t*)(sf + 4);
        float* tp = *(float**)sf;
        for (int32_t i = 0; i < n && i < 0x600; i++) {
            if (chance(30)) continue;
            uint32_t* w = (uint32_t*)&tp[3 * i];
            w[1] = chance(g_nans ? 50 : 92) ? fbits(range(-0.2f, 1.2f)) : chance(60) ? wild_bits() : fbits(range(-1e6f, 1e6f));
            w[2] = chance(g_nans ? 50 : 92) ? fbits(range(-0.2f, 1.2f)) : chance(60) ? wild_bits() : fbits(range(-1e6f, 1e6f));
        }
    }
}
static void random_frame(uint32_t a) {
    for (int i = 0; i < 9; i++) UI_GU32(a + 4 * i) = fval_bits(-1.0f, 1.0f);
    for (int i = 9; i < 12; i++) UI_GU32(a + 4 * i) = fval_bits(-10.0f, 10.0f);
}
static void randomize_world() {
    void* b = W.builders[chance(10) ? 0 : chance(25) ? 1 : chance(50) ? 2 : 3];
    UI_GP(void, S_BUILDER) = b;
    UI_GP(void, S_UNDO) = W.undo;
    randomize_points(b);
    const int32_t nsurf = b_count(b, 0x1c), nvert = b_count(b, 4), ntri = b_count(b, 0x10);
    UI_G32(S_SURFACE) = chance(85) ? irange(0, nsurf > 0 ? nsurf - 1 : 0) : irange(-2, nsurf + 1);
    int32_t ntp = 0;
    if (UI_G32(S_SURFACE) >= 0 && UI_G32(S_SURFACE) < nsurf)
        ntp = *(int32_t*)(*(uint8_t**)((uint8_t*)b + 0x18) + 0x2c * UI_G32(S_SURFACE) + 4);
    UI_G32(S_VERTEX) = chance(80) ? irange(-1, nvert > 0 ? nvert - 1 : 0) : irange(-3, nvert + 3);
    UI_G32(S_TPOINT) = chance(80) ? irange(-1, ntp > 0 ? ntp - 1 : 0) : irange(-3, ntp + 3);
    UI_G32(S_TRIANGLE) = chance(80) ? irange(-1, ntri > 0 ? ntri - 1 : 0) : irange(-3, ntri + 3);
    for (int i = 0; i < 3; i++) UI_G32(S_SEL + 4 * i) = chance(85) ? irange(-1, ntp > 0 ? ntp - 1 : 0) : irange(-2, ntp + 2);
    if (chance(20) && ntp >= 3) { UI_G32(S_SEL) = 0; UI_G32(S_SEL + 4) = 1; UI_G32(S_SEL + 8) = 2; }
    UI_G32(S_MODE) = chance(90) ? irange(0, 5) : chance(50) ? -1 : 6;
    UI_G32(S_TV_TOOL) = chance(92) ? irange(0, 3) : 4;
    UI_G32(S_LOCK) = irange(0, 3);
    UI_G32(S_ZOOM) = irange(0, 6);
    UI_G32(S_UNITS) = irange(0, 2);
    UI_G8(S_MODIFIED) = (uint8_t)(rnd() & 1);
    UI_G8(S_XFORM_SET) = (uint8_t)(chance(70));
    random_frame(S_XFORM);
    if (chance(30)) {                                    // the identity, or near it
        for (int i = 0; i < 12; i++) UI_GU32(S_XFORM + 4 * i) = (i % 4 == 0 && i < 9) ? 0x3f800000u : 0;
        if (chance(50)) UI_GU32(S_XFORM + 0x24) = fbits(range(-0.5f, 0.5f));
    }
    random_frame(S_FRAME);
    random_frame(S_CAMERA);
    for (uint32_t a : {S_HAXIS, S_VAXIS}) {
        UI_G32(a) = chance(8) ? 0 : irange(1, 1300);
        UI_G32(a + 4) = irange(0, 300);
        UI_G32(a + 8) = chance(90) ? irange(0, 1100) : irange(-50, 3000);
    }
    UI_GU32(S_SLIDER) = fval_bits(0.0f, 20.0f);
    UI_GU32(S_BLACK) = rnd(); UI_GU32(S_PT_COLOR) = chance(20) ? UI_GU32(S_EDGE_COLOR) : rnd(); UI_GU32(S_SEL_COLOR) = rnd();
    if (chance(80)) UI_GU32(S_EDGE_COLOR) = rnd();
    {
        static const char* const files[] = {"untitled.mod", "C:\\models\\car.mod", "noext", "a.b.c", "x"};
        strcpy((char*)ED_P(S_FILE), files[rnd() % 5]);
    }
    strcpy((char*)ED_P(S_COORD_TEXT), "()");
    UI_GP(char, S_TEXNAME) = W.texname;
    UI_G32(S_NEW_SURFACE) = irange(-1, 3);
    // the views
    ModelViewer* mv = ed_mv();
    mv->yaw_b = fval_bits(-3.5f, 3.5f);
    mv->pitch_b = fval_bits(-1.8f, 1.8f);
    mv->dist_b = chance(15) ? fbits(range(0.0f, 0.01f)) : fval_bits(0.0f, 20.0f);
    mv->drag = (uint8_t)chance(75);
    mv->mx = coord(0, 640); mv->my = coord(0, 480);
    mv->capture = chance(50) ? 0 : 0x333;
    for (int i = 0; i < 12; i++) ((volatile uint32_t*)&mv->marker)[i] = fval_bits(-2.0f, 2.0f);
    TextureViewer* tv = ed_tv();
    tv->u_b = fval_bits(-0.2f, 1.2f);
    tv->v_b = fval_bits(-0.2f, 1.2f);
    tv->down = (uint8_t)chance(80);
    tv->zoom_b = chance(85) ? fbits((float)irange(1, 5)) : wild_bits();
    tv->tex_size = chance(90) ? 1 << irange(4, 8) : irange(-1, 1);
    tv->noverlays = chance(80) ? irange(0, 10) : irange(-1, 12);
    tv->overlay = chance(80) ? irange(-1, 10) : irange(-3, 12);
    for (int i = 0; i < 10; i++) {
        tv->counts[i] = chance(90) ? irange(0, 12) : irange(-2, 30);
        for (int k = 0; k < 32; k++) {
            ((volatile uint32_t*)&tv->pts[i][k])[0] = chance(90) ? fbits(range(-0.1f, 1.1f)) : wild_bits();
            ((volatile uint32_t*)&tv->pts[i][k])[1] = chance(90) ? fbits(range(-0.1f, 1.1f)) : wild_bits();
        }
    }
    if (chance(30) && tv->overlay >= 0 && tv->overlay < 10 && tv->counts[tv->overlay] > 0) {   // a point the mouse lands near
        tv->pts[tv->overlay][0].u = range(0.3f, 0.7f);
        tv->pts[tv->overlay][0].v = range(0.3f, 0.7f);
    }
    // the stubs' world
    HS->time = (int32_t)rnd();
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->script_n = HS->script_pos = 0;
    HS->scan = chance(50) ? 0 : rnd();
    HS->seed = rnd();
    HS->grab_ok = chance(80);
    HS->tex_size = chance(90) ? 1 << irange(5, 8) : irange(0, 1);
    HS->save_ok = chance(80);
    HS->write_ok = chance(80);
    HS->find_ok = chance(50);
    HS->import_ok = chance(75);
    HS->box_ok = chance(85);
    HS->model_next = 0; HS->tex_next = 0; HS->picks = 0;
}
// .data after poisoning, and each round: the statics the code follows
static void restore_followed(const Mem& from) {
    auto keep = [&](uint32_t a, uint32_t n) { memcpy(DATA + (a - 0x004e1000), from.data + (a - 0x004e1000), n); };
    keep(S_MV, sizeof(ModelViewer));
    keep(S_TV, 0x84);
    keep(S_INFO, sizeof(EdInfo));
    keep(S_INFO2, sizeof(EdInfo));
    keep(S_BG_CANVAS, sizeof(gxCanvas));
    keep(S_CANVASES, S_CANVASES_END - S_CANVASES);
}
static void randomize_statics() {
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_GU32(S_DIALOG_IDLE) = 0;
    UI_G8(S_EXIT) = 0;
    UI_GF(S_UI_DT) = range(0.0f, 0.25f);
    UI_GP(void, S_CURSOR) = 0;
    UI_G32(S_MOUSE_X) = irange(0, 639); UI_G32(S_MOUSE_Y) = irange(0, 479);
    UI_G32(S_SCREEN_W) = 640; UI_G32(S_SCREEN_H) = 480;
    UI_GP(char, 0x00504340) = W.carlist;
    UI_G32(0x00504344) = 6;
}
// a random input script (for the functions that run a dialog)
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x1a, 0x13, 9, 0x18, 0xe, 0xf, 0x123, 0x125, 'q', '5', '1', '.'};
    const int n = irange(0, maxops);
    for (int i = 0; i < n && i < 62; i++) {
        int32_t* op = HS->script[i];
        const uint32_t r = rnd() % 12;
        op[1] = op[2] = 0;
        if (r < 4) { op[0] = OP_MOVE_W; op[1] = irange(0, 1000); op[2] = irange(0, 479); }
        else if (r < 5) { op[0] = OP_MOVE_XY; op[1] = irange(-10, 650); op[2] = irange(-10, 490); }
        else if (r < 6) op[0] = OP_DOWN;
        else if (r < 7) op[0] = OP_UP;
        else if (r < 8) op[0] = OP_RDOWN;
        else if (r < 9) op[0] = OP_RUP;
        else if (r < 11) { op[0] = OP_KEY; op[1] = keys[rnd() % (sizeof keys / sizeof keys[0])]; }
        else op[0] = OP_NOP;
        if (op[0] == OP_MOVE_W && chance(70)) {
            const bool right = chance(15);
            HS->script[i + 1][0] = right ? OP_RDOWN : OP_DOWN;
            HS->script[i + 1][1] = HS->script[i + 1][2] = 0;
            HS->script[i + 2][0] = right ? OP_RUP : OP_UP;
            HS->script[i + 2][1] = HS->script[i + 2][2] = 0;
            i += 2;
        }
        HS->script_n = i + 1;
    }
}

// ---- poison: .data/.bss random but for what's read as a string or followed as a pointer ------------------------------------------
static void poison(const Mem& keepfrom) {
    uint32_t* d = (uint32_t*)DATA;
    for (uint32_t i = 0; i < DATA_BYTES / 4; i++) d[i] = rnd();
    static const uint32_t keep[][2] = {
        {0x004e4db8, 0x004e4dbc},             // ""
        {0x004eb108, 0x004eb10c},             // Xlator::g_cookie
        {0x004f665c, 0x004f6698},             // the ui library's Xlators' keys
        {0x004f6b78, 0x004f7228}, {0x004f7230, 0x004f7300},   // ui / _widget / widget / uistyle strings
        {0x004efbf8, 0x004efbfc},             // the current canvas
        {0x004fe8f4, 0x004ff200},             // modtool.obj's and modbuild.obj's strings (its .data statics set again below)
        {0x005024b8, 0x005024bc},             // __adjust_fdiv
        {0x005228d4, 0x005228d8}, {0x005228f4, 0x005228f8},   // the screen's size
        {0x00578cd0, 0x00578cdc}, {0x00578d50, 0x00578d5c}, {0x00578d60, 0x00578d6c}, {0x00578e88, 0x00578e94},
        {0x00578e98, 0x00578ea4}, {0x00578eb0, 0x00578ebc}, {0x00578ec0, 0x00578ecc}, {0x00578ef8, 0x00578f04},
        {0x00579050, 0x0057905c},             // the ui library's Xlators
        {0x00578d08, 0x00578d40},             // the end item
        {0x00578d48, 0x00578d4c}, {0x00578e78, 0x00578e7c}, {0x00578f40, 0x00578f44}, {0x00578e80, 0x00578e84},
        {0x005790e0, 0x005796e0},             // the styles
        {0x00579730, 0x00579778},             // the running window, the window table
    };
    for (auto& k : keep) memcpy(DATA + (k[0] - 0x004e1000), keepfrom.data + (k[0] - 0x004e1000), k[1] - k[0]);
    memcpy(DATA + (0x00502000 - 0x004e1000), keepfrom.data + (0x00502000 - 0x004e1000), 0x1000);   // the CRT's own data
    restore_followed(keepfrom);
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static uint32_t b8(uint32_t v) { return chance(50) ? (rnd() & ~0xffu) | (v & 0xff) : v; }
static uint32_t xy(int32_t base, int32_t span) { return (uint32_t)(chance(85) ? base + irange(-10, span + 10) : coord(-500, 1500)); }
static void random_mat(float* m, int n) { for (int i = 0; i < n; i++) ((uint32_t*)m)[i] = chance(g_nans ? 50 : 85) ? fbits(range(-2.0f, 2.0f)) : wild_bits(); }
// the builder in use's surface s: its texture points {vertex, u, v} and their count (0 if there's no such surface)
static int32_t surf_points(int32_t s, const uint8_t** tp) {
    void* b = UI_GP(void, S_BUILDER);
    if (s < 0 || s >= b_count(b, 0x1c)) return 0;
    const uint8_t* sf = *(uint8_t**)((uint8_t*)b + 0x18) + 0x2c * s;
    *tp = *(uint8_t**)sf;
    return *(const int32_t*)(sf + 4);
}
static int32_t surf_ttris(int32_t s, const int32_t** tt) {
    void* b = UI_GP(void, S_BUILDER);
    if (s < 0 || s >= b_count(b, 0x1c)) return 0;
    const uint8_t* sf = *(uint8_t**)((uint8_t*)b + 0x18) + 0x2c * s;
    *tt = *(int32_t**)(sf + 0xc);
    return *(const int32_t*)(sf + 0x10);
}
// a surface with three texture points or more shown, three of them selected (the 3-point tools' case)
static void choose_sel3() {
    void* b = UI_GP(void, S_BUILDER);
    const int32_t ns = b_count(b, 0x1c);
    for (int k = 0; k < 8; k++) {
        const int32_t s = ns > 0 ? irange(0, ns - 1) : 0;
        const uint8_t* tp;
        const int32_t n = surf_points(s, &tp);
        if (n < 3) continue;
        UI_G32(S_SURFACE) = s;
        const int32_t a = irange(0, n - 1);
        int32_t c = irange(0, n - 1), d = irange(0, n - 1);
        if (c == a) c = (a + 1) % n;
        if (d == a || d == c) d = (c + 1) % n == a ? (c + 2) % n : (c + 1) % n;
        UI_G32(S_SEL) = a; UI_G32(S_SEL + 4) = c; UI_G32(S_SEL + 8) = d;
        if (chance(30)) UI_G32(S_TPOINT) = d;
        return;
    }
}
// the window position of the shown surface's texture point i (TextureViewer's pixel: total * u - pos + the view's x)
static bool point_pixel(int32_t i, int32_t* x, int32_t* y) {
    const uint8_t* tp;
    const int32_t n = surf_points(UI_G32(S_SURFACE), &tp);
    if (i < 0 || i >= n) return false;
    const float u = *(const float*)(tp + 12 * i + 4), v = *(const float*)(tp + 12 * i + 8);
    if (!(fabsf(u) < 100.0f) || !(fabsf(v) < 100.0f)) return false;
    *x = (int32_t)((double)UI_G32(S_HAXIS) * u) - UI_G32(S_HAXIS + 8) + ed_tv()->x + irange(-6, 6);
    *y = (int32_t)((double)UI_G32(S_VAXIS) * v) - UI_G32(S_VAXIS + 8) + ed_tv()->y + irange(-6, 6);
    return true;
}
// the world nudged toward the paths a function has (each still random)
static void bias(const char* nm, uint32_t* a) {
    auto is = [&](const char* s) { return !strcmp(nm, s); };
    if (is("set_transform") || is("three_point_put_cb") || is("plane_proj_cb") || is("TextureViewer::Draw") ||
        (is("TextureViewer::CharHit") && ((a[0] & 0xffff) == 's' || (a[0] & 0xffff) == 'S'))) {
        if (chance(75)) { UI_G32(S_MODE) = 4; choose_sel3(); }
    }
    if (is("ModelViewer::MouseDown")) {
        if (chance(85)) UI_G32(S_MODE) = irange(1, 4);
        if (chance(80)) HS->scan &= ~4u;                              // Ctrl (0xfe) up
        if (UI_G32(S_MODE) == 4 && chance(80)) { choose_sel3(); UI_G8(S_XFORM_SET) = 1; }
    }
    if (is("ModelViewer::MouseMove") && chance(90)) ed_mv()->drag = 1;
    if (is("ModelViewer::CharHit") && chance(80)) {
        UI_G32(S_MODE) = 2;
        const int32_t* tt;
        const int32_t n = surf_ttris(UI_G32(S_SURFACE), &tt);
        if (n > 0 && chance(80)) UI_G32(S_TRIANGLE) = tt[4 * irange(0, n - 1)];
    }
    if (is("TextureViewer::MouseDown")) {
        if (chance(25)) { UI_G32(S_TV_TOOL) = 0; UI_G32(S_MODE) = chance(80) ? 1 : 2; }
        else if (UI_G32(S_TV_TOOL) == 0) UI_G32(S_TV_TOOL) = 1;
        const uint8_t* tp;
        const int32_t n = surf_points(UI_G32(S_SURFACE), &tp);
        int32_t x, y;
        if (n > 0 && chance(75) && point_pixel(irange(0, n - 1), &x, &y)) { a[0] = (uint32_t)x; a[1] = (uint32_t)y; }
    }
    if (is("TextureViewer::MouseMove") || is("TextureViewer::TranslatePoints")) {
        if (chance(90)) ed_tv()->down = 1;
        const uint8_t* tp;
        const int32_t n = surf_points(UI_G32(S_SURFACE), &tp);
        if (n > 0 && chance(85)) UI_G32(S_TPOINT) = irange(0, n - 1);
        if (UI_G32(S_LOCK) == 3 || chance(25)) {                      // the overlay: points near where the mouse is
            UI_G32(S_LOCK) = 3;
            TextureViewer* tv = ed_tv();
            const int32_t ov = irange(0, 9);
            tv->noverlays = 10;
            tv->overlay = ov;
            const int32_t ht = UI_G32(S_HAXIS), vt = UI_G32(S_VAXIS);
            if (ht && vt) {
                const float u = (float)(UI_G32(S_HAXIS + 8) - tv->x + (int32_t)a[0]) / (float)ht;
                const float v = (float)(UI_G32(S_VAXIS + 8) - tv->y + (int32_t)a[1]) / (float)vt;
                const int32_t c = tv->counts[ov] > 0 && tv->counts[ov] <= 32 ? tv->counts[ov] : (tv->counts[ov] = 4);
                for (int32_t i = 0; i < c && i < 32; i++)
                    if (chance(60)) { tv->pts[ov][i].u = u + range(-0.06f, 0.06f); tv->pts[ov][i].v = v + range(-0.06f, 0.06f); }
            }
        }
    }
    if (is("TextureViewer::CharHit") && (a[0] & 0xffff) == 8) {
        const uint8_t* tp;
        const int32_t n = surf_points(UI_G32(S_SURFACE), &tp);
        if (n > 0 && chance(80)) UI_G32(S_TPOINT) = irange(0, n - 1);
    }
}
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const char* nm = f.name;
    uint32_t* a = f.fast ? w + 2 : w;
    memset(W.scratch, 0xa5, 0x2000);
#define IS(s) (!strcmp(nm, s))
    const TextureViewer* tv = ed_tv();
    if (strstr(nm, "deleting destructor")) {
        w[0] = U(W.scratch);
        a[0] = rnd() & 3;
    } else if (!strncmp(nm, "TextureViewer::", 15)) {
        w[0] = S_TV;
        if (IS("TextureViewer::CharHit")) {
            static const uint16_t k[] = {8, 'S', 's', 'a', 0x12e, 0x1b, 0x74, 0};
            a[0] = chance(50) ? (rnd() & 0xffff0000u) | k[rnd() % 8] : rnd();
        } else if (IS("TextureViewer::Draw")) a[0] = chance(70) ? U(W.screen) : U(W.canvas2);
        else if (IS("TextureViewer::AddPoint")) { a[0] = xy(tv->x, tv->w); a[1] = xy(tv->y, tv->h); a[2] = (uint32_t)irange(-1, 30); }
        else if (IS("TextureViewer::GetTextureU") || IS("TextureViewer::GetTextureV")) a[0] = xy(tv->x, tv->w);
        else if (IS("TextureViewer::Callback")) { a[0] = rnd() & 1; a[1] = S_VAXIS; }
        else { a[0] = xy(tv->x, tv->w); a[1] = xy(tv->y, tv->h); }
    } else if (!strncmp(nm, "ModelViewer::", 13)) {
        w[0] = S_MV;
        if (IS("ModelViewer::CharHit")) {
            static const uint16_t k[] = {8, 0x12e, 's', 0x1b};
            a[0] = chance(70) ? (rnd() & 0xffff0000u) | k[rnd() % 4] : rnd();
        } else if (IS("ModelViewer::Draw")) a[0] = chance(70) ? U(W.screen) : U(W.canvas2);
        else if (IS("ModelViewer::set_position")) { a[0] = fval_bits(-3.5f, 3.5f); a[1] = fval_bits(-1.8f, 1.8f); a[2] = fval_bits(0.0f, 20.0f); }
        else if (IS("ModelViewer::Callback")) { a[0] = 0; a[1] = S_MODE; }
        else { a[0] = xy(ed_mv()->x, ed_mv()->w); a[1] = xy(ed_mv()->y, ed_mv()->h); }
    } else if (IS("MatrixSet(rows)(modtool.obj)")) {
        random_mat(W.mats, 64);
        a[0] = U(W.mats); a[1] = U(W.mats + 16); a[2] = U(W.mats + 20); a[3] = chance(20) ? U(W.mats + 16) : U(W.mats + 24);
    } else if (IS("MatrixInverse(modtool.obj)")) {
        random_mat(W.mats, 64);
        if (chance(20)) for (int i = 0; i < 9; i++) W.mats[16 + i] = (i % 4 == 0) ? range(0.5f, 2.0f) : range(-0.1f, 0.1f);
        if (chance(10)) for (int i = 0; i < 9; i++) W.mats[16 + i] = 0;
        a[0] = U(W.mats); a[1] = U(W.mats + 16);
    } else if (IS("MatrixSet(floats)(modtool.obj)")) { random_mat(W.mats, 64); a[0] = U(W.mats); a[1] = U(W.mats + 16); }
    else if (IS("snap")) {
        random_mat(W.mats, 64);
        if (chance(20)) W.mats[1] = W.mats[0];
        if (chance(20)) W.mats[2] = chance(50) ? W.mats[0] : -W.mats[1];
        a[0] = U(W.mats);
    } else if (IS("get_textured_point")) { a[0] = (uint32_t)irange(-2, 30); a[1] = U(W.scratch); a[2] = U(W.scratch + 16); a[3] = U(W.scratch + 20); }
    else if (IS("select_tp") || IS("is_selected")) a[0] = chance(50) ? (uint32_t)UI_G32(S_SEL + 4 * (rnd() % 3)) : (uint32_t)irange(-2, 30);
    else if (IS("set_transform")) a[0] = b8(rnd() & 1);
    else if (IS("change_surface")) a[0] = (uint32_t)irange(-2, 6);
    else if (IS("idle_func")) a[0] = U(W.scratch);
    else if (IS("set_model")) a[0] = chance(50) ? S_INFO : S_INFO2;
    else if (IS("build_edge_model")) a[0] = chance(70) ? S_INFO2 : U(W.info3);
    else if (IS("copy_model_info")) {
        a[0] = chance(50) ? U(W.info3) : S_INFO2;
        a[1] = chance(50) ? S_INFO : S_INFO2;
        if (a[0] == a[1]) a[1] = S_INFO;
    } else if (IS("create_model_info")) a[0] = U(W.scratch);
    else if (IS("destroy_model_info")) a[0] = U(W.info_fake);
    else if (IS("load_mod") || IS("dxf_import") || IS("ad3ds_import") || IS("save_mod") || IS("ModTool")) a[0] = U(W.names[rnd() % 6]);
    else if (IS("switch_overlay_cb")) a[0] = (uint32_t)irange(0, 3);
    else a[0] = (uint32_t)irange(-3, 8);               // the button callbacks (an id)
#undef IS
    bias(nm, a);
    return true;
}
static bool is_modal(const char* nm) {
    static const char* const m[] = {"ModTool", "EditMenu", "exit_cb(modtool.obj)", "export_cb(modtool.obj)", "open_cb", "translate_cb", "scale_cb", "set_texture_cb",
                                    "template_cb(modtool.obj)", "browse_cb", "import_cb(modtool.obj)", "prompt_save", "save_file", "save_mod", "measure_cb", "save_cb(modtool.obj)"};
    for (const char* s : m)
        if (!strcmp(nm, s)) return true;
    return false;
}

// ---- main -----------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    const int rounds = argc > 1 ? atoi(argv[1]) : 24;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    g_nans = GetEnvironmentVariableA("VP_NANS", 0, 0) != 0;
    const char* only = getenv("VP_ONLY");
    char exe[MAX_PATH], inv[MAX_PATH], install[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_edit_tool.cpp");
    if (s) *s = 0;
    sprintf(inv, "%s\\out\\inventory.csv", exe);
    sprintf(install, "%s\\..\\game-files\\installs\\v1.0-RC", exe);
    strcat(exe, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    read_inventory(inv);
    GetCurrentThreadStackLimits(&g_stack_lo, &g_stack_hi);
    {
        HMODULE m = GetModuleHandleA(0);
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
        g_self_lo = (uintptr_t)m; g_self_hi = (uintptr_t)m + nt->OptionalHeader.SizeOfImage;
    }
    struct { uint32_t at; void* to; } stubs[] = {
        {0x004cf730, (void*)&stub_crt_fatal}, {0x004d45b0, (void*)&stub_crt_fatal}, {0x004d4570, (void*)&stub_crt_fatal},
        {0x004d5c10, (void*)&stub_crt_fatal}, {0x004d6850, (void*)&stub_crt_msgbox},
        {F_MemAlloc, (void*)&stub_MemAlloc}, {F_Delete, (void*)&stub_delete}, {F_LogReport, (void*)&stub_LogReport},
        {F_LogPanic, (void*)&stub_LogPanic}, {uit::F_SingleBegin, (void*)&stub_SingleBegin}, {uit::F_SingleEnd, (void*)&stub_SingleEnd},
        {uit::F_atexit, (void*)&stub_atexit}, {uit::F_PTimeNow, (void*)&stub_PTimeNow}, {uit::F_MouseGetEvent, (void*)&stub_MouseGetEvent},
        {uit::F_KeyHit, (void*)&stub_KeyHit}, {uit::F_KeyGet, (void*)&stub_KeyGet}, {uit::F_MousePeek, (void*)&stub_MousePeek},
        {uit::F_MouseClear, (void*)&stub_MouseClear}, {uit::F_KeyClear, (void*)&stub_KeyClear}, {F_ScanDown, (void*)&stub_ScanDown},
        {F_gxGetStamp, (void*)&stub_gxGetStamp}, {F_gxForgetStamp, (void*)&stub_gxForgetStamp},
        {uit::F_gxStampWidth, (void*)&stub_gxStampWidth}, {uit::F_gxStampHeight, (void*)&stub_gxStampHeight},
        {uit::F_gxStampCount, (void*)&stub_gxStampCount}, {uit::F_gxStampHotSpot, (void*)&stub_gxStampHotSpot},
        {uit::F_gxStampHitTest, (void*)&stub_gxStampHitTest}, {F_gxDrawStamp, (void*)&stub_gxDrawStamp},
        {uit::F_gxFontGet, (void*)&stub_gxFontGet}, {uit::F_gxFontForget, (void*)&stub_gxFontForget},
        {uit::F_gxFontHeight, (void*)&stub_gxFontHeight}, {uit::F_gxFontAscent, (void*)&stub_gxFontAscent},
        {uit::F_gxFontDescent, (void*)&stub_gxFontDescent}, {uit::F_gxFontStringWidth, (void*)&stub_gxFontStringWidth},
        {uit::F_gxFontStringWidthN, (void*)&stub_gxFontStringWidthN}, {uit::F_gxFontPrintN, (void*)&stub_gxFontPrintN},
        {uit::F_gxFontPrintf, (void*)&stub_gxFontPrintf}, {uit::F_gxPaletteCreate, (void*)&stub_gxPaletteCreate},
        {uit::F_gxPaletteDestroy, (void*)&stub_gxPaletteDestroy}, {uit::F_gxPaletteMakeGradient, (void*)&stub_gxPaletteMakeGradient},
        {F_gxSetCanvas, (void*)&stub_gxSetCanvas}, {F_gxClear, (void*)&stub_gxClear}, {F_gxRect, (void*)&stub_gxRect},
        {F_gxLine, (void*)&stub_gxLine}, {F_gxPoint, (void*)&stub_gxPoint}, {F_gxXORPoint, (void*)&stub_gxXORPoint},
        {F_gxCircle, (void*)&stub_gxCircle}, {F_gxTriangle, (void*)&stub_gxTriangle}, {F_gxPasteZoom, (void*)&stub_gxPasteZoom},
        {0x00451500, (void*)&stub_gxPaste}, {0x00451550, (void*)&stub_gxPasteAlpha}, {0x00450020, (void*)&stub_gxClearNoAlpha},
        {0x00452c50, (void*)&stub_gxXORLine}, {uit::F_gxText, (void*)&stub_gxText}, {uit::F_gxTextHeight, (void*)&stub_gxTextHeight},
        {F_gxAllocCanvas, (void*)&stub_gxAllocCanvas3}, {F_gxFreeCanvas, (void*)&stub_gxFreeCanvas},
        {uit::F_gxGrabScreen, (void*)&stub_gxGrabScreen}, {uit::F_gxReleaseScreen, (void*)&stub_gxReleaseScreen},
        {uit::F_gxFlip, (void*)&stub_gxFlipScreen}, {uit::F_mrBeginFrame, (void*)&stub_mrBeginFrame}, {uit::F_mrEndFrame, (void*)&stub_mrEndFrame},
        {F_gxGetTexture, (void*)&stub_gxGetTexture}, {F_gxForgetTexture, (void*)&stub_gxForgetTexture},
        {F_gxFreeAllTextures, (void*)&stub_gxFreeAllTextures}, {F_gxGetTextureSize, (void*)&stub_gxGetTextureSize},
        {F_gxGrabTexture, (void*)&stub_gxGrabTexture}, {F_gxReleaseTexture, (void*)&stub_gxReleaseTexture},
        {F_gxWriteBMP, (void*)&stub_gxWriteBMP},
        {F_mrSetView, (void*)&stub_mrSetView}, {F_mrSetProjection, (void*)&stub_mrSetProjection},
        {F_mrModelSetClipDist, (void*)&stub_mrModelSetClipDist}, {F_mrSetCamera, (void*)&stub_mrSetCamera},
        {F_mrModelDraw, (void*)&stub_mrModelDraw}, {F_mrModelLoad, (void*)&stub_mrModelLoad}, {F_mrModelUnload, (void*)&stub_mrModelUnload},
        {F_mrModelCreate, (void*)&stub_mrModelCreate}, {F_mrModelDestroy, (void*)&stub_mrModelDestroy},
        {F_mrModelBuild, (void*)&stub_mrModelBuild}, {F_mrLightCalc, (void*)&stub_mrLightCalc}, {F_mrModelPick, (void*)&stub_mrModelPick},
        {F_mrModelPickVertex, (void*)&stub_mrModelPickVertex}, {F_mrModelPickEdge, (void*)&stub_mrModelPickEdge},
        {F_mrModelGetExtents, (void*)&stub_mrModelGetExtents}, {F_mrModelInfoSave, (void*)&stub_mrModelInfoSave},
        {uit::F_Xlator_xlate, (void*)&stub_xlate}, {0x0041aec0, (void*)&stub_Xlate}, {F_sprintf, (void*)&stub_sprintf},
        {F_atof, (void*)&stub_atof}, {uit::F_LocaleConvertNumeric, (void*)&stub_LocaleConvertNumeric}, {uit::F_tolower, (void*)&stub_tolower},
        {uit::F_ClipboardGetText, (void*)&stub_ClipboardGetText},
        {F_FileFindFirst, (void*)&stub_FileFindFirst}, {uit::F_FileFindNext, (void*)&stub_FileFindNext},
        {F_FileFindClose, (void*)&stub_FileFindClose},
        {F_UIDoOpenFileBox, (void*)&stub_file_box}, {F_UIDoSaveFileBox, (void*)&stub_file_box},
        {F_ResourceSetMustLoad, (void*)&stub_ResourceSetMustLoad}, {F_ResourceSetUnload, (void*)&stub_ResourceSetUnload},
        {F_ModBuilderImportDXF, (void*)&stub_ImportDXF}, {F_ModBuilderImportMOD, (void*)&stub_ImportMOD},
        {F_ModBuilderImport3DS, (void*)&stub_Import3DS}, {F_Write3DS, (void*)&stub_Write3DS},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    const bool cover = GetEnvironmentVariableA("VP_COVER", 0, 0) != 0;
    struct { uint32_t at; void* to; } covs[] = {
        {F_ModBuilderAddTextureTriangle, (void*)&cov_AddTextureTriangle}, {F_ModBuilderMoveTexturePoint, (void*)&cov_MoveTexturePoint},
        {F_ModBuilderSetTriangle, (void*)&cov_SetTriangle}, {F_ModBuilderFacetAll, (void*)&cov_FacetAll},
        {F_ModBuilderSmoothAll, (void*)&cov_SmoothAll}, {F_ModBuilderDeleteTextureTriangle, (void*)&cov_DeleteTextureTriangle},
        {F_ModBuilderDeleteTexturePoint, (void*)&cov_DeleteTexturePoint}, {F_ModBuilderTranslateModel, (void*)&cov_TranslateModel},
        {F_ModBuilderScaleModel, (void*)&cov_ScaleModel}, {F_ModBuilderSetSurfaceGroup, (void*)&cov_SetSurfaceGroup},
        {F_ModBuilderAddTexturePoint, (void*)&cov_AddTexturePoint},
    };
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    memset(g_arena + A_HEAP, 0xee, ARENA_BYTES - A_HEAP);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    build_world(install);
    if (cover)                                           // (after the world is built: it needs the real ones)
        for (auto& st : covs) patch_jmp(st.at, st.to);
    g_hw = (HS->heap_max + 0x1fff) & ~0xfffu;
    mem_save(g_pristine, ARENA_BYTES);
    printf("world: %u $E initialisers run, %d widgets in the window, %u KB of heap used, the stock model %s\n", (unsigned)g_setup_e.size(),
           W.win->count, (HS->heap_next - A_HEAP) / 1024, W.stock_name[0] ? W.stock_name : "(none: generated)");

    int dup = 0;
    for (int i = 0; i < g_nfns; i++)
        for (int j = i + 1; j < g_nfns; j++)
            if (g_fns[i].v10 == g_fns[j].v10 || !strcmp(g_fns[i].name, g_fns[j].name)) {
                printf("  listed twice: %08x %s / %08x %s\n", g_fns[i].v10, g_fns[i].name, g_fns[j].v10, g_fns[j].name);
                dup++;
            }

    static Footprint fp;
    long long checks = 0, poisoned_checks = 0, log_words = 0;
    int differ = 0, fp_bad = 0, faults = 0, pure_n = 0, replay_n = 0, changed_fns = 0, still_fns = 0, bad_fns = 0, skipped = 0;
    long long both_fault = 0, fp_checks = 0;
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0, fn_fpck = 0, fn_rchanged = 0;
        long long fn_words = 0;
        const bool estat = f.name[0] == '$';
        const int nr = is_modal(f.name) ? rounds : estat ? 2 * rounds : rounds * 10;
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine, g_hw);
            if (poisoned) poison(g_pristine);
            randomize_world();
            randomize_statics();
            random_script(is_modal(f.name) || !strcmp(f.name, "ModelViewer::MouseDown") || !strcmp(f.name, "TextureViewer::CharHit") ? 24 : 4);
            uint32_t words[72];
            if (!make_args(f, words)) { skipped++; continue; }
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            const uint32_t hw0 = g_hw;
            HS->heap_max = HS->heap_next;
            mem_save(g_snap, hw0);
            const Result ro = run(f, false, words);
            const uint32_t hw1 = ((HS->heap_max > hw0 ? HS->heap_max : hw0) + 0xfff) & ~0xfffu;
            const bool changed = mem_diff(g_snap, hw0) != 0 || hw1 > hw0;
            fn_changed |= changed;
            fn_rchanged += changed;
            if (!fp.replay_only && !ro.fault) {
                fn_fpck++;
                fp_checks++;
                uint32_t out = 0;
                auto covered = [&](const uint8_t* p) {
                    for (int k = 0; k < fp.n; k++) if (p >= (uint8_t*)fp.r[k].p && p < (uint8_t*)fp.r[k].p + fp.r[k].n) return true;
                    return false;
                };
                for (uint32_t i2 = next_diff(g_snap.data, DATA, DATA_BYTES, 0); i2 < DATA_BYTES && !out; i2 = next_diff(g_snap.data, DATA, DATA_BYTES, i2 + 1))
                    if (fp.pure || !covered(DATA + i2)) out = 0x004e1000 + i2;
                for (uint32_t i2 = next_diff(g_snap.arena, g_arena, hw0, sizeof(HState)); i2 < hw0 && !out; i2 = next_diff(g_snap.arena, g_arena, hw0, i2 + 1))
                    if (fp.pure || !covered(g_arena + i2)) out = U(g_arena + i2);
                if (out) {
                    printf("  FOOTPRINT %08x %s: the original changed %08x (arena +%x) outside it (round %d)\n", f.v10, f.name, out,
                           out - U(g_arena), rd);
                    if (getenv("VP_DEBUG_WORLD"))
                        for (int k = 0; k < fp.n; k++) printf("    footprint %08x +%x %s\n", U(fp.r[k].p), fp.r[k].n, fp.r[k].what);
                    fp_bad++;
                    fn_bad = true;
                }
            }
            mem_save(g_after, hw1);
            memcpy(&g_log_orig, &g_log, sizeof(uint32_t) * (1 + (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)));
            mem_load(g_snap, hw0);
            if (hw1 > hw0) memcpy(g_arena + hw0, g_pristine.arena + hw0, hw1 - hw0);   // (the pattern the original's pass found there)
            const Result rn = run(f, true, words);
            const uint32_t hw2 = ((HS->heap_max > hw1 ? HS->heap_max : hw1) + 0xfff) & ~0xfffu;
            if (hw2 > hw1) memcpy(g_after.arena + hw1, g_pristine.arena + hw1, hw2 - hw1);
            checks++;
            fn_checks++;
            poisoned_checks += poisoned;
            log_words += g_log_orig.n;
            fn_words += g_log_orig.n;
            if (ro.fault || rn.fault) { faults++; fn_faults++; }
            if (ro.fault && rn.fault) both_fault++;
            bool same = ro.fault == rn.fault && ro.code == rn.code && ro.ret == rn.ret && ro.pops == rn.pops && ro.top == rn.top &&
                        !memcmp(&ro.st0, &rn.st0, sizeof ro.st0) && !memcmp(ro.regs, rn.regs, sizeof ro.regs);
            const uint32_t where = mem_diff(g_after, hw2);
            same &= where == 0;
            same &= g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
            if (hw2 > g_hw) g_hw = hw2;
            if (!same) {
                printf("  MISMATCH %08x %s (round %d, %s, pc %s)\n", f.v10, f.name, rd, poisoned ? "poisoned" : "as loaded", g_pc == _PC_24 ? "24" : "53");
                printf("    fault %d/%d (%08x at %08x / %08x at %08x), return %08x / %08x, x87 top %u / %u, popped %u / %u,"
                       " ebx esi edi ebp %08x %08x %08x %08x / %08x %08x %08x %08x\n",
                       ro.fault, rn.fault, ro.code, ro.eip, rn.code, rn.eip, ro.ret, rn.ret, ro.top, rn.top, ro.pops, rn.pops,
                       ro.regs[0], ro.regs[1], ro.regs[2], ro.regs[3], rn.regs[0], rn.regs[1], rn.regs[2], rn.regs[3]);
                if (memcmp(&ro.st0, &rn.st0, sizeof ro.st0)) printf("    st0 %.17g / %.17g\n", ro.st0, rn.st0);
                if (where) {
                    uint32_t a2 = where & ~3u, o = 0, n2 = 0;
                    if (a2 >= 0x004e1000 && a2 < 0x004e1000 + DATA_BYTES) { memcpy(&o, g_after.data + (a2 - 0x004e1000), 4); memcpy(&n2, (void*)(uintptr_t)a2, 4); }
                    else if (a2 >= U(g_arena) && a2 < U(g_arena) + ARENA_BYTES) { memcpy(&o, g_after.arena + (a2 - U(g_arena)), 4); memcpy(&n2, (void*)(uintptr_t)a2, 4); }
                    printf("    memory first differs at %08x (arena +%x): %08x / %08x\n", where, where - U(g_arena), o, n2);
                }
                if (g_log.n != g_log_orig.n) printf("    the stub logs differ: %u / %u words\n", g_log_orig.n, g_log.n);
                for (uint32_t i = 0; i < g_log.n && i < g_log_orig.n && i < LOG_MAX; i++)
                    if (g_log.w[i] != g_log_orig.w[i]) {
                        printf("    the stub logs differ at word %u: %08x / %08x (context:", i, g_log_orig.w[i], g_log.w[i]);
                        for (uint32_t k = i >= 6 ? i - 6 : 0; k < i + 3 && k < g_log.n; k++) printf(" %08x/%08x", g_log_orig.w[k], g_log.w[k]);
                        printf(")\n");
                        break;
                    }
                if (getenv("VP_DUMP")) {
                    printf("    the scratch floats:");
                    for (int i = 0; i < 26; i++) printf(" %08x", ((const uint32_t*)g_snap.arena)[(U(W.mats) - U(g_arena)) / 4 + i]);
                    printf("\n");
                    for (uint32_t i = 0; i < g_log_orig.n || i < g_log.n; i++) {
                        const uint32_t a = i < g_log_orig.n ? g_log_orig.w[i] : 0, b = i < g_log.n ? g_log.w[i] : 0;
                        const char* tag = (a >> 24) >= 'A' && (a >> 24) <= 'Z' ? "  <" : "";
                        printf("    %4u %08x %08x%s%s\n", i, a, b, a != b ? "  *" : "", tag);
                    }
                }
                differ++;
                fn_bad = true;
            }
            if (rd == 0) { pure_n += fp.pure; replay_n += fp.replay_only != 0; }
        }
        bad_fns += fn_bad;
        (fn_changed ? changed_fns : still_fns)++;
        if (cover) {
            printf("  coverage %s:", f.name);
            for (int k = 0; k < NCOV; k++) if (g_cov[k]) printf(" %s %ld", k_cov_names[k], g_cov[k]);
            printf("\n");
            memset(g_cov, 0, sizeof g_cov);
        }
        if (trace)
            printf("%08x %-50s %s%s (%d checks, %d changed memory, %d with the footprint checked, %d faulted, %lld words logged a check)\n",
                   f.v10, f.name, fn_bad ? "BAD" : "ok", fn_changed ? "" : " (never changed memory)", fn_checks, fn_rchanged, fn_fpck,
                   fn_faults, fn_checks ? fn_words / fn_checks : 0);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine, g_hw);
    printf("%d functions (%d pure, %d replay_only in round 0, %d others), %d listed twice; %lld checks (%lld on poisoned .data, %lld "
           "with the footprint checked), %lld logged words compared: %d differ, %d footprint violations, %d checks faulted (%lld in "
           "both, the same way), %d skipped; %d functions bad\n", g_nfns, pure_n, replay_n, g_nfns - pure_n - replay_n, dup, checks,
           poisoned_checks, fp_checks, log_words, differ, fp_bad, faults, both_fault, skipped, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    return differ || fp_bad || dup ? 1 : 0;
}
