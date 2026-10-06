// world_paintkit.cpp -- M3 UI stage, step U4 (group C): every rewrite of hook/paint_kit.cpp, hook/paint_main.cpp and
// hook/paint_tga.cpp (library `paintkit`: paintkit.obj, tga.obj) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_paintkit.cpp
//        /Fo<dir>\ /Fe<dir>\world_paintkit.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86 /STACK:0x800000
//   run:   world_paintkit.exe [rounds] [seed]          (VP_TRACE=1: one line per function; VP_ONLY=text: those only;
//          VP_DEBUG_WORLD=1: the world's objects and every fault's registers)
//     (and with /DVP_PAINT_FIXES: the fix build, below)
//
// Loads out\race_v10.exe at 0x400000 the way test/world_menu_view.cpp does (a child process with the range reserved) and
// includes the three rewrite files with PORT_FN redefined to list each function (its v1.0 address, the rewrite, its calling
// convention, stack arguments and return, its footprint). VP_FAITHFUL: the rewrites exactly as the originals.
//
// The world. Once: the $E static initialisers of libraries ui and paintkit (their Xlators, colours), the game's UIBegin
// (the styles); a car list for the root's GetCarFileName / GetMaxCarFileNames (the originals run); the car "viper"; then the
// paint kit's objects: a PaintKitCanvas by its own original constructor (its tools, its four canvases -- the painting's
// pixels real, 256 x 256 x 4 in the arena --, the brushes: brush.stp's four canvases from the stubbed gxCanvasGet and the
// cut brush, also real), a PaintCarViewer3D by its constructor, a ColorChooser, a ColorIndicator and a ColorBucket as
// paint_main builds them, three TemplateLayers by their constructor with template_cb's colours, a TemplatePreview, a
// DecalViewer with eight sets as decal_cb fills them; all put in a full-screen window by the game's WidgetCreateWindow and
// _UIAddItems (adding one runs its Added: the layers' buckets), then their Create. This is the pristine state.
//
// Then for EVERY function, `rounds` times (ten times that for one that doesn't run a dialog or a loop): pristine restored;
// half the rounds POISONED (.data/.bss random but for the strings and tables the code reads and what the ui library
// follows); then the world's state random: the canvas's fields (the zoom 0..6, the dirty rectangle, the mouse, the flags,
// the paint job), its tools' points and colours, the viewer's yaw and pitch (NaN / infinite / huge now and then), the
// controls' rectangles, the templates (0..32 stamps, the one shown -1..n), the decal sets (counts, sizes, 0s among them),
// the paint kit's statics (tool -1..9, brush 0..5, colours, scroll axes, the cut brush's size, the car and whether it's the
// viper), every function-local Xlator built or not (fresh or stale); arguments made for the function (the right tool or
// control, points around its rectangle and past it, canvases, shapes' draw modes 0..3, destructor flags, file names); for
// the dialogs (paint_main, PaintKitDo, the template / decal / save / default / import / export boxes) a random input script
// ended by a watchdog. The stack filled with one pattern after the FPU is set; the ORIGINAL runs from a raw call thunk; its
// results kept, the snapshot restored, the REWRITE runs. Compared: .data/.bss/.idata and the 2 MB arena (a pointer into this
// thread's stack in both counts as equal), the return value, the x87 depth, bytes popped, ebx / esi / edi / ebp, faults,
// and every stub's call log. Every byte the original changed must lie in the rewrite's footprint (unless replay_only); a
// pure one changes nothing. The x87 at 24 or 53 bits (alternating rounds).
//
// Stubbed (logged; state in the arena so both passes see the same): world_menu_view.cpp's set (memory, logs, input and
// time, the 2D calls, Xlator::xlate, sprintf, the controls, files) and the paint kit's callees outside the ui library: every
// gfx draw it makes (paste, paste-alpha, paste-zoom, clear, clear-no-alpha, rect, line, point, XOR line, brush line,
// triangle, cut -- which fills the cut brush's pixels --, get-pixel, flip, mirror, rotate, make-brush), the canvases
// (gxAllocCanvas: real pixels for the painting and the cut brush, a read-only pattern for the rest; gxFreeCanvas,
// gxCanvasGet / Forget / Read / Write), the car's texture (gxCreateTexture, Destroy, Grab, Release), the 3D renderer, the
// kernel's files (FileCreate / Open / ReadExact -- a TGA header matching the canvas or not, rows of a pattern -- / Write /
// Close, FileCreateDirectory), Win32GetUserDirectory, ScanDown, resources (ResourceExists by the name and the round,
// MustLoad, Unload), the decals' StringTable, Xlate, atoi, stricmp. The game's own code everywhere else: the widget
// toolkit (UIDoDialog, _UIAddItems, the boxes, WidgetExit, the notifications), UIDialogItem's and Xlator's constructors,
// UICustomControl::Dirty / AddNotification / AddItems, gxSetClip / gxRestoreClip, the matrix functions, GetCarFileName /
// GetMaxCarFileNames, and every function of this group (each rewrite is checked against its original with the same
// callees).
//
// Built with /DVP_PAINT_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Paint kit").
// Every function is still compared as above; a round that reaches a fixed case -- a car name over 31 characters handed to
// PaintKitDo or paint_begin, S_CAR (or a car list name) too long for the original's 0x20-byte names, a path too long for
// the 0x104-byte buffers, a decal set's name of 256 or more characters (or one that can't be read to its end) shown by
// DecalViewer, a decals.tab name of 238 or more characters read by decal_cb -- is kept out of the comparison and counted, the rewrite still run on it and required to return cleanly.
// (The random states never make one: the cars are short, the user directory 22 characters, the sets' names short.) Then
// directed_fix_tests: for each fix, the bad case run on the rewrite alone -- no fault, the bytes popped and ebx / esi /
// edi / ebp kept, nothing written outside what it may write (every other byte of .data/.bss/.idata and the arena compared
// with before), and the result the fix promises (the names the stubs are handed, the log's text, the buffer's bytes) --
// and its boundary case (the longest input that fits) on both, compared bit for bit. Without it (VP_FAITHFUL) every
// rewrite must match its original bit for bit.
#define _CRT_SECURE_NO_WARNINGS
#if defined(__GNUC__) && !defined(__clang__)
#define _WIN32_WINNT 0x0A00               // (mingw: GetCurrentThreadStackLimits)
#endif
#include <windows.h>
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

#ifndef VP_PAINT_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#define PAINT_FIXES 0
#else
#define PAINT_FIXES 1               // the fix build (above)
#endif
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif
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
static Ent g_fns[256];
static int g_nfns;
struct EntReg { EntReg(const Ent& e) { if (g_nfns < 256) g_fns[g_nfns++] = e; } };
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

#include "../hook/paint_kit.cpp"
#include "../hook/paint_main.cpp"
#include "../hook/paint_tga.cpp"

using namespace pkit;
using uit::Widget; using uit::WidgetWindow; using uit::CustomWidget; using uit::UIDialogItem; using uit::UIScrollAxis;
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
static float wildf() {
    switch (rnd() % 8) {
    case 0: return bitsf(chance(50) ? 0x7fc00000u : 0xffc00000u);
    case 1: return (rnd() & 1) ? INFINITY : -INFINITY;
    case 2: return chance(50) ? 0.0f : -0.0f;
    case 3: return range(-1e30f, 1e30f);
    case 4: return bitsf(rnd() & 0x807fffff);
    default: return range(-100.0f, 100.0f);
    }
}
static float fval(float lo, float hi) { return chance(12) ? wildf() : range(lo, hi); }

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
static uint8_t* const DATA = (uint8_t*)0x004e1000;      // .data and its .bss
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* const IDATA = (uint8_t*)0x005d7000;
enum { IDATA_BYTES = 0x136a };
enum { ARENA_BYTES = 0x200000 };
static uint8_t* g_arena;
struct Mem { uint8_t* data; uint8_t* idata; uint8_t* arena; };
static Mem g_pristine, g_snap, g_after;
static Mem mem_alloc() { return {(uint8_t*)malloc(DATA_BYTES), (uint8_t*)malloc(IDATA_BYTES), (uint8_t*)malloc(ARENA_BYTES)}; }
static void mem_save(Mem& m) { memcpy(m.data, DATA, DATA_BYTES); memcpy(m.idata, IDATA, IDATA_BYTES); memcpy(m.arena, g_arena, ARENA_BYTES); }
static void mem_load(const Mem& m) { memcpy(DATA, m.data, DATA_BYTES); memcpy(IDATA, m.idata, IDATA_BYTES); memcpy(g_arena, m.arena, ARENA_BYTES); }
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
static uint32_t mem_diff(const Mem& m) {
    uint32_t i = block_diff(m.data, DATA, DATA_BYTES, 0);
    if (i < DATA_BYTES) return 0x004e1000 + i;
    i = block_diff(m.idata, IDATA, IDATA_BYTES, 0);
    if (i < IDATA_BYTES) return 0x005d7000 + i;
    i = block_diff(m.arena, g_arena, ARENA_BYTES, 0);
    if (i < ARENA_BYTES) return (uint32_t)(uintptr_t)(g_arena + i);
    return 0;
}

// ---- the stubs' state (in the arena, so each pass starts from the same) ------------------------------------------------------
enum { OP_NOP, OP_MOVE_W, OP_MOVE_XY, OP_DOWN, OP_UP, OP_RDOWN, OP_RUP, OP_KEY };
struct HState {
    uint32_t heap_next;
    int32_t time, frames, frame_limit;
    int32_t script_n, script_pos;
    int32_t script[64][3];
    int32_t mouse_x, mouse_y;
    int32_t pal_next, alloc_canvas_next, grab_fail_at;
    // the paint kit's stubs
    uint32_t res_mask;                          // ResourceExists: a name answers by its hash and this
    int32_t scan;                               // ScanDown
    int32_t tex_ok;                             // gxGrabTexture
    int32_t file_ok;                            // FileCreate / FileOpen give a handle
    int32_t writes, write_fail_at;              // FileWrite: the call that fails
    int32_t reads, read_fail_at;                // FileReadExact: the call that fails
    int32_t hdr_w, hdr_h, hdr_bad;              // the TGA header read: its size, which field is off (0: none)
    int32_t canvas_read_ok, canvas_write_ok;
    int32_t rows;                               // StringTableNumRows
    int32_t counts[8];                          // decals.tab's column 0
    int32_t model_next, tex_next;
    uint32_t pix_seed;                          // gxGetPixel's answers
};
#define HS ((HState*)g_arena)
enum : uint32_t { A_HEAP = 0x100000 };

// ---- the log ----------------------------------------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 16 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void L(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static uintptr_t g_self_lo, g_self_hi;
static uint8_t* g_pool;                                  // the read-only pixels of the canvases that aren't real
enum { POOL_BYTES = 0x80000 };
// is [p, p + n) memory this program may read (the arena, the image, this program, this thread's stack, the pool)?
static bool readable(const void* p, uint32_t n) {
    const uint64_t a = (uint64_t)(uintptr_t)p, e = a + (uint64_t)n;
    if (a >= (uintptr_t)g_arena && e <= (uint64_t)(uintptr_t)g_arena + ARENA_BYTES) return true;
    if (a >= 0x401000 && e <= 0x5d9000) return true;
    if (a >= g_self_lo && e <= g_self_hi) return true;
    if (g_pool && a >= (uintptr_t)g_pool && e <= (uint64_t)(uintptr_t)g_pool + POOL_BYTES) return true;
    if (n && on_stack((uint32_t)a) && e - 1 <= 0xffffffffull && on_stack((uint32_t)(e - 1))) return true;
    return false;
}
static bool in_arena(const void* p, uint32_t n) {
    const uint64_t a = (uint64_t)(uintptr_t)p, e = a + (uint64_t)n;
    return a >= (uintptr_t)g_arena && e <= (uint64_t)(uintptr_t)g_arena + ARENA_BYTES;
}
static uint32_t P(const void* p) { return on_stack((uint32_t)(uintptr_t)p) ? 'STAK' : (uint32_t)(uintptr_t)p; }
// (the fix tests: the names and texts the stubs are handed, recorded while g_fx_rec is on)
struct FxNote { uint32_t tag; char s[0x400]; };
static FxNote g_fx_notes[96];
static int g_fx_nn;
static bool g_fx_rec;
static void fx_note(uint32_t tag, const char* s) {
    if (!g_fx_rec || g_fx_nn >= 96) return;
    FxNote& r = g_fx_notes[g_fx_nn++];
    r.tag = tag;
    int i = 0;
    if (s && readable(s, 1))
        for (; i < 0x3ff && readable(s + i, 1) && s[i]; i++) r.s[i] = s[i];
    r.s[i] = 0;
}
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

// ---- stubs: memory, logs, sync ---------------------------------------------------------------------------------------------------
static void* __cdecl stub_MemAlloc(int n) {
    L('ALOC'); L((uint32_t)n);
    uint32_t a = (HS->heap_next + 7) & ~7u;
    if (a < A_HEAP || a + (uint32_t)(n > 0 ? n : 0) > ARENA_BYTES - 0x100 || n < 0 || n > 0x80000) { L('OOM '); return 0; }
    HS->heap_next = a + (uint32_t)n;
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
static void __cdecl stub_LogReport(const char* fmt, ...) {
    L('LOGR');
    va_list ap;
    va_start(ap, fmt);
    L_va(fmt, (const uint32_t*)ap);
    if (g_fx_rec && fmt && readable(fmt, 1)) {                     // (the fix tests: the message as the game would print it)
        static char m[0x800];
        _vsnprintf(m, sizeof m - 1, fmt, ap);
        m[sizeof m - 1] = 0;
        fx_note('LOGR', m);
    }
    va_end(ap);
}
static void __cdecl stub_LogPanic(const char* fmt, ...) { L('PANC'); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); }
static int __cdecl stub_SingleBegin(const char* s) { L('SBEG'); LS(s); return 0x5151; }
static void __cdecl stub_SingleEnd(int h, const char*, int) { L('SEND'); L((uint32_t)h); }
static int __cdecl stub_atexit(uint32_t fn) { L('ATEX'); L(fn); return 0; }
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }
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
static uint8_t __cdecl stub_ScanDown(uint32_t k) { L('SCAN'); L(k & 0xff); return (uint8_t)HS->scan; }

// ---- stubs: the 2D calls --------------------------------------------------------------------------------------------------------
struct Fake { uint32_t magic; char name[32]; int32_t w, h, count, hx, hy, asc, desc, cw; };
static Fake g_fakes[64];
static int g_nfakes;
static Fake* fake_for(const char* name, uint32_t magic) {
    char nm[32] = "?";
    if (name && readable(name, 1)) { strncpy(nm, name, 31); nm[31] = 0; }
    for (int i = 0; i < g_nfakes; i++) if (g_fakes[i].magic == magic && !strcmp(g_fakes[i].name, nm)) return &g_fakes[i];
    if (g_nfakes >= 64) return &g_fakes[0];
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
static void __cdecl stub_gxClearNoAlpha(uint32_t c) { L('CLRN'); L(c); L_canvas(); }
static void __cdecl stub_gxRect(int32_t a, int32_t b, int32_t c, int32_t d, uint32_t col) { L('RECT'); L(a); L(b); L(c); L(d); L(col); L_canvas(); }
static void __cdecl stub_gxLine(int32_t a, int32_t b, int32_t c, int32_t d, uint32_t col) { L('LINE'); L(a); L(b); L(c); L(d); L(col); L_canvas(); }
static void __cdecl stub_gxXORLine(int32_t a, int32_t b, int32_t c, int32_t d) { L('XLIN'); L(a); L(b); L(c); L(d); L_canvas(); }
static void __cdecl stub_gxBrushLine(int32_t a, int32_t b, int32_t c, int32_t d, const gxCanvas* br) { L('BLIN'); L(a); L(b); L(c); L(d); L(P(br)); L_canvas(); }
static void __cdecl stub_gxPoint(int32_t x, int32_t y, uint32_t col) { L('PNT '); L(x); L(y); L(col); L_canvas(); }
static void __cdecl stub_gxTriangle(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f, uint32_t col) {
    L('TRI '); L(a); L(b); L(c); L(d); L(e); L(f); L(col); L_canvas();
}
static uint32_t __cdecl stub_gxGetPixel(int32_t x, int32_t y) {
    L('GPIX'); L(x); L(y); L_canvas();
    return ((uint32_t)x * 2654435761u) ^ ((uint32_t)y * 40503u) ^ HS->pix_seed;
}
static void __cdecl stub_gxPaste(const gxCanvas* src, int32_t x, int32_t y) { L('PAST'); L(P(src)); L((uint32_t)x); L((uint32_t)y); L_canvas(); }
static void __cdecl stub_gxPasteAlpha(const gxCanvas* src, int32_t x, int32_t y) { L('PASA'); L(P(src)); L((uint32_t)x); L((uint32_t)y); L_canvas(); }
static void __cdecl stub_gxPasteZoom(const gxCanvas* src, int32_t x, int32_t y, uint32_t fx, uint32_t fy) {
    L('PASZ'); L(P(src)); L((uint32_t)x); L((uint32_t)y); L(fx); L(fy); L_canvas();
}
// gxCut: the canvas's w x h filled (as a copy of the current canvas at x, y would), so the cut brush's pixels change
static void __cdecl stub_gxCut(gxCanvas* dst, int32_t x, int32_t y) {
    L('CUT '); L(P(dst)); L((uint32_t)x); L((uint32_t)y); L_canvas();
    if (!dst || !readable(dst, sizeof(gxCanvas))) return;
    const int32_t w = dst->w, h = dst->h;
    L((uint32_t)w); L((uint32_t)h);
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || !in_arena(dst->pixels, (uint32_t)(w * h * 4))) return;
    uint32_t* p = (uint32_t*)dst->pixels;
    for (int32_t i = 0; i < w * h; i++) p[i] = ((uint32_t)(x + i) * 2654435761u) ^ (uint32_t)y ^ HS->pix_seed;
}
static void __cdecl stub_gxFlip(gxCanvas* c) { L('FLIP'); L(P(c)); }
static void __cdecl stub_gxMirror(gxCanvas* c) { L('MIRR'); L(P(c)); }
static void __cdecl stub_gxRotate90(gxCanvas* c) { L('ROT '); L(P(c)); }
static void __cdecl stub_gxMakeBrush(gxCanvas* c, const void* s, uint32_t col, int32_t fr) { L('MKBR'); L(P(c)); L(P(s)); L(col); L((uint32_t)fr); }
static void __cdecl stub_gxText(int32_t x, int32_t y, const char* s, uint32_t col) { L('TEXT'); L((uint32_t)x); L((uint32_t)y); LS(s); L(col); L_canvas(); }
static int32_t __cdecl stub_gxTextHeight() { L('TXTH'); return 8; }
// the window's canvases (gxAllocCanvas(w, h)): pixels no one reads
static void __cdecl stub_gxAllocCanvas3(gxCanvas* c, int32_t w, int32_t h) {
    L('ACNV'); L(P(c)); L((uint32_t)w); L((uint32_t)h);
    c->format = 4; c->flags = 0; c->pixels = (uint8_t*)(uintptr_t)(0x7c000000u + (uint32_t)(HS->alloc_canvas_next++) * 0x100000u);
    c->w = w; c->h = h; c->pitch = w * 2; c->cx0 = 0; c->cy0 = 0; c->cx1 = w; c->cy1 = h;
}
// gxAllocCanvas(w, h, format): the painting's and the cut brush's pixels real (from the heap), the rest the read-only pool
static gxCanvas* g_real_canvas;                          // the world's painting (set before its constructor runs)
static void __cdecl stub_gxAllocCanvas4(gxCanvas* c, int32_t w, int32_t h, int32_t fmt) {
    L('ACN4'); L(P(c)); L((uint32_t)w); L((uint32_t)h); L((uint32_t)(uint8_t)fmt);
    const int32_t bpp = (int8_t)fmt == 5 ? 4 : 2;
    c->format = (int8_t)fmt; c->flags = 0; c->w = w; c->h = h; c->pitch = w * bpp;
    c->cx0 = 0; c->cy0 = 0; c->cx1 = w; c->cy1 = h;
    c->pixels = g_pool;
    if ((c == (gxCanvas*)PK_P(S_BRUSH_CANVAS) || c == g_real_canvas) && w > 0 && h > 0 && w <= 256 && h <= 256) {
        uint8_t* p = (uint8_t*)stub_MemAlloc(w * h * bpp);
        if (p) { memset(p, 0x5a, (size_t)(w * h * bpp)); c->pixels = p; }
    }
}
static void __cdecl stub_gxFreeCanvas(gxCanvas* c) { L('FCNV'); L(P(c)); }
static uint8_t __cdecl stub_gxGrabScreen(gxCanvas* c) {
    L('GRAB'); L(P(c));
    if (HS->frames == HS->grab_fail_at) return 0;
    c->format = 4; c->flags = 0x80; c->pixels = (uint8_t*)(uintptr_t)0x7b000000u; c->w = 640; c->h = 480; c->pitch = 1280;
    c->cx0 = 0; c->cy0 = 0; c->cx1 = 640; c->cy1 = 480;
    return 1;
}
static void __cdecl stub_gxReleaseScreen() { L('RELS'); }
static void __cdecl stub_gxFlipScreen() { L('FLPS'); }
static void __cdecl stub_mrBeginFrame() { L('MRBF'); }
static void __cdecl stub_mrEndFrame() { L('MREF'); }
// the resource canvases (gxCanvasGet): one per name, made on first use in the heap, sized by the name
static gxCanvas* __cdecl stub_gxCanvasGet(const char* name) {
    L('CGET'); LS(name);
    fx_note('CGET', name);
    const uint32_t h = hash_str(name);
    // a name's canvas: brushes small squares, the colour bar 50 x 162, the shading and mask 256 x 256, decals 32 wide
    int32_t w = 16 + (int32_t)(h % 48), hh = 16 + (int32_t)((h >> 8) % 48);
    if (name && readable(name, 6)) {
        if (!strncmp(name, "brush", 5)) w = hh = 1 + (int32_t)(h % 24);
        else if (!strcmp(name, "colorbar.cvs")) { w = 50; hh = 162; }
        else if (!strcmp(name, "shade.cvs") || !strcmp(name, "mask.cvs") || !strncmp(name, "paint", 5)) w = hh = 256;
        else if (!strncmp(name, "decals", 6)) { w = 16 + 16 * (int32_t)(h % 3); hh = w * (1 + (int32_t)((h >> 4) % 9)); }
    }
    gxCanvas* c = (gxCanvas*)stub_MemAlloc(sizeof(gxCanvas));
    if (!c) return 0;
    c->format = 5; c->flags = 0; c->pixels = g_pool; c->w = w; c->h = hh; c->pitch = w * 4;
    c->cx0 = 0; c->cy0 = 0; c->cx1 = w; c->cy1 = hh;
    L((uint32_t)w); L((uint32_t)hh);
    return c;
}
static void __cdecl stub_gxCanvasForget(gxCanvas* c) { L('CFGT'); L(P(c)); }
static uint8_t __cdecl stub_gxCanvasRead(gxCanvas* c, const char* name) { L('CRD '); L(P(c)); LS(name); fx_note('CRD ', name); return (uint8_t)HS->canvas_read_ok; }
static uint8_t __cdecl stub_gxCanvasWrite(const gxCanvas* c, const char* name) { L('CWR '); L(P(c)); LS(name); fx_note('CWR ', name); return (uint8_t)HS->canvas_write_ok; }
static int32_t __cdecl stub_gxCreateTexture(const char* name, int32_t a, int32_t b) {
    L('CTEX'); LS(name); L((uint32_t)a); L((uint32_t)b);
    fx_note('CTEX', name);
    return 0x70 + HS->tex_next++;
}
static void __cdecl stub_gxDestroyTexture(int32_t t) { L('DTEX'); L((uint32_t)t); }
static uint8_t __cdecl stub_gxGrabTexture(int32_t t, gxCanvas* c) {
    L('GTEX'); L((uint32_t)t); L(P(c));
    if (!HS->tex_ok) return 0;
    c->format = 4; c->flags = 0; c->pixels = g_pool; c->w = 256; c->h = 256; c->pitch = 512;
    c->cx0 = 0; c->cy0 = 0; c->cx1 = 256; c->cy1 = 256;
    return 1;
}
static void __cdecl stub_gxReleaseTexture() { L('RTEX'); }
// the 3D renderer
static void __cdecl stub_mrSetView(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t c) { L('MRSV'); L(x); L(y); L(w); L(h); L(c); }
static void __cdecl stub_mrSetProjection(uint32_t a, uint32_t b, uint32_t c) { L('MRSP'); L(a); L(b); L(c); }
static void __cdecl stub_mrModelSetClipDist(uint32_t a, uint32_t b) { L('MRCD'); L(a); L(b); }
static void __cdecl stub_mrSetCamera(const void* fr) { L('MRSC'); L_block(fr, 12); }
static void __cdecl stub_mrPushState() { L('MRPU'); }
static void __cdecl stub_mrPopState() { L('MRPO'); }
static void __cdecl stub_mrEnable(int32_t f) { L('MREN'); L((uint32_t)f); }
static void __cdecl stub_mrModelEnvMap(uint8_t on) { L('MREM'); L(on); }
static void __cdecl stub_mrModelDraw(int32_t m, const void* fr) { L('MRDR'); L((uint32_t)m); L_block(fr, 12); }
static int32_t __cdecl stub_mrModelLoad(const char* name) { L('MRLD'); LS(name); fx_note('MRLD', name); return 0x300 + HS->model_next++; }
static void __cdecl stub_mrModelUnload(int32_t m) { L('MRUL'); L((uint32_t)m); }

// ---- stubs: text, files, resources ------------------------------------------------------------------------------------------------
static const char* const k_xl_text[16] = {"Back", "Ok", "Cancel", "Paint Kit", "Default", "Template", "Import", "Export", "Decals",
                                          "Save changes?", "Are you sure?", "File changed", "Preview", "Export failed", "Import failed",
                                          "A longer translation of a paint kit text"};
static uint32_t g_xl_seed;
static const char* xl_text(uint32_t key) {
    const uint32_t h = hash_str((const char*)(uintptr_t)key) ^ g_xl_seed;
    return k_xl_text[(h & 0xff) < 0xf0 ? h % 15 : 15];
}
static void __fastcall stub_xlate(uint32_t* xl, int) {
    L('XLAT'); L(P(xl));
    xl[1] = (uint32_t)(uintptr_t)xl_text(xl[0]);
    xl[2] = UI_GU32(S_XLATOR_COOKIE);
}
static const char* __cdecl stub_Xlate(const char* s) { L('XLTE'); LS(s); fx_note('XLTE', s); return k_xl_text[hash_str(s) % 16]; }
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
    fx_note('SPRF', buf);
    return r;
}
static void __cdecl stub_LocaleConvertNumeric(char* s) { L('LCNV'); LS(s); }
static int __cdecl stub_tolower(int c) { L('TLOW'); L((uint32_t)c); return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static void __cdecl stub_ControlToDisplayString(char* buf, int n, const uint32_t* ctl) {
    L('CTDS'); L(P(buf)); L((uint32_t)n); L(P(ctl));
    if (n > 0) { strncpy(buf, "ctl", (size_t)n - 1); buf[n - 1] = 0; }
}
static void __cdecl stub_ControlUpdate(uint8_t u) { L('CUPD'); L(u); }
static void __cdecl stub_ControlDetectInit() { L('CDIN'); }
static uint8_t __cdecl stub_ControlDetect(uint32_t* ctl) { L('CDET'); L(P(ctl)); return 0; }
static void* __cdecl stub_FileFindFirst(const char* pat, char*, int n) { L('FFF '); LS(pat); L((uint32_t)n); return (void*)(intptr_t)-1; }
static uint8_t __cdecl stub_FileFindNext(void* h, char*, int) { L('FFN '); L((uint32_t)(uintptr_t)h); return 0; }
static void __cdecl stub_FileFindClose(void* h) { L('FFC '); L((uint32_t)(uintptr_t)h); }
static uint8_t __cdecl stub_ClipboardGetText(char* buf, int n) { L('CLIP'); if (n > 0) buf[0] = 0; return 0; }
// the files: handles by the round; writes logged by their bytes' hash; reads a TGA header (matching the canvas, or with
// one field off) and rows of a pattern
static int32_t __cdecl stub_FileCreate(const char* name) { L('FCRE'); LS(name); fx_note('FCRE', name); return HS->file_ok ? 0x1234 : 0; }
static int32_t __cdecl stub_FileOpen(const char* name) { L('FOPN'); LS(name); fx_note('FOPN', name); return HS->file_ok ? 0x2345 : 0; }
static void __cdecl stub_FileClose(int32_t* fd) { L('FCLS'); L((uint32_t)*fd); *fd = 0; }
static uint8_t __cdecl stub_FileWrite(int32_t fd, const void* p, int32_t n) {
    L('FWRT'); L((uint32_t)fd); L((uint32_t)n); L(hash_bytes(p, (uint32_t)(n > 0 ? n : 0)));
    return ++HS->writes == HS->write_fail_at ? 0 : 1;
}
static uint8_t __cdecl stub_FileReadExact(int32_t fd, void* p, int32_t n) {
    L('FRDX'); L((uint32_t)fd); L(P(p)); L((uint32_t)n);
    if (++HS->reads == HS->read_fail_at) return 0;
    if (n <= 0 || n > 0x10000 || !readable(p, (uint32_t)n)) return 1;
    uint8_t* b = (uint8_t*)p;
    if (n == 0x12) {                                               // the header
        memset(b, 0, 0x12);
        b[2] = 2;
        *(uint16_t*)(b + 0xc) = (uint16_t)HS->hdr_w;
        *(uint16_t*)(b + 0xe) = (uint16_t)HS->hdr_h;
        b[0x10] = 0x18;
        b[0x11] = 8;
        switch (HS->hdr_bad) {
        case 1: b[0x10] = 0x20; break;
        case 2: b[1] = 1; break;
        case 3: b[0] = 5; break;
        case 4: *(uint16_t*)(b + 8) = 3; break;
        case 5: *(uint16_t*)(b + 0xa) = 7; break;
        case 6: b[2] = 10; break;                                  // (image type: not compared)
        }
        return 1;
    }
    for (int32_t i = 0; i < n; i++) b[i] = (uint8_t)((i * 37) ^ HS->reads ^ (int32_t)HS->pix_seed);
    return 1;
}
static uint8_t __cdecl stub_FileCreateDirectory(const char* name) { L('FCDR'); LS(name); fx_note('FCDR', name); return 1; }
static char g_user_dir[0x104] = "C:\\Games\\Viper\\Config\\";          // (the game's is 0x104 bytes)
static const char* __cdecl stub_Win32GetUserDirectory() { L('UDIR'); return g_user_dir; }
static uint8_t __cdecl stub_ResourceExists(const char* name) {
    L('REXI'); LS(name);
    fx_note('REXI', name);
    const uint32_t h = hash_str(name) ^ HS->res_mask;
    return (uint8_t)((h >> 3) % 5 != 0);                           // (4 in 5 exist)
}
static void __cdecl stub_ResourceSetMustLoad(const char* s) { L('RSML'); LS(s); fx_note('RSML', s); }
static void __cdecl stub_ResourceSetUnload(const char* s) { L('RSUL'); LS(s); }
static uint8_t g_table[4];
static void* __cdecl stub_StringTableGet(const char* name) { L('STGT'); LS(name); return g_table; }
static int32_t __cdecl stub_StringTableNumRows(const void* t) { L('STNR'); L(P(t)); return HS->rows; }
static char g_entries[8][2][24];
static const char* g_fx_entry1;                                    // (the fix tests: every row's column 1)
static const char* __cdecl stub_StringTableGetEntry(const void* t, int32_t row, int32_t col) {
    L('STEN'); L(P(t)); L((uint32_t)row); L((uint32_t)col);
    if (col == 1 && g_fx_entry1) return g_fx_entry1;
    char* e = g_entries[(uint32_t)row & 7][col & 1];
    if (col == 0) sprintf(e, "%d", HS->counts[(uint32_t)row & 7]);
    else sprintf(e, "Set%d", row);
    return e;
}
static void __cdecl stub_StringTableForget(void* t) { L('STFG'); L(P(t)); }
static int __cdecl stub_stricmp(const char* a, const char* b) {
    L('SICM'); LS(a); LS(b);
    if (!readable(a, 1) || !readable(b, 1)) { *(volatile int*)0 = 0; }
    const int r = _stricmp(a, b);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
static int32_t __cdecl stub_atoi(const char* s) { L('ATOI'); LS(s); return readable(s, 1) ? atoi(s) : 0; }

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
        "1:\n\t"                                          // pushloop
        "test ecx, ecx\n\t"
        "jz 2f\n\t"
        "dec ecx\n\t"
        "push dword ptr [%P[c_args] + ecx * 4]\n\t"
        "jmp 1b\n"
        "2:\n\t"                                          // pushed
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
        :: [save_esp] "i"(&g_save_esp), [c_n] "i"(&g_c_n), [c_args] "i"(g_c_args), [c_top] "i"(&g_c_top),
           [c_ecx] "i"(&g_c_ecx), [c_edx] "i"(&g_c_edx), [c_fn] "i"(&g_c_fn), [r_eax] "i"(&g_r_eax), [r_edx] "i"(&g_r_edx),
           [r_ebx] "i"(&g_r_ebx), [r_esi] "i"(&g_r_esi), [r_edi] "i"(&g_r_edi), [r_ebp] "i"(&g_r_ebp), [r_esp] "i"(&g_r_esp),
           [r_sw] "i"(&g_r_sw), [r_st0] "i"(&g_r_st0));
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
    return EXCEPTION_EXECUTE_HANDLER;
}
// the stack below the call filled with one pattern (write_tex's frame alone is 0x2ab10 bytes)
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
static void* wv(uint32_t n) { g_wv = (g_wv + 7) & ~7u; void* p = g_arena + g_wv; g_wv += n; memset(p, 0, n); return p; }
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

struct World {
    gxCanvas* screen;
    gxCanvas* canvas2;
    WidgetWindow* win;
    PaintKitCanvas* g;
    PaintCarViewer3D* viewer;
    ColorChooser* cc;
    ColorIndicator* ci;
    ColorBucket* bucket;
    TemplateLayer* layers;                            // 3
    TemplatePreview* preview;
    DecalViewer* decal;
    uint32_t* bucket_color;
    uint8_t* scratch;                                 // fresh blocks for constructors (0x1000)
    uint16_t* mip;                                    // make_mip's output (0x20000)
    gxCanvas* tga;                                    // a 16 x 8 canvas with real pixels (the TGAs)
    gxCanvas* tex16;                                  // a 256 x 256 16-bit canvas (write_tex, make_mip)
    TGAHeader* hdr[2];                                // compare_headers's
    char* carlist;
    char* names[6];                                   // file names for the TGAs / write_tex / PaintKitDo
    gxCanvas* brushes[4];                             // brush.stp's (the constructor's gxCanvasGet)
    uint8_t* brush_pixels;
    void* stamps[32];                                 // the templates
    std::vector<Widget*> widgets;
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
        if ((!strcmp(c[2], "ui") || !strcmp(c[2], "paintkit")) && c[5][0] == '$' && c[5][1] == 'E') g_setup_e.push_back((uint32_t)strtoul(c[0], 0, 16));
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
static const char* k_cars[] = {"viper", "cobra", "gts", "rt10", "stingray", "zr1"};
static void build_world() {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    HS->res_mask = 0;
    HS->file_ok = 1;
    HS->canvas_read_ok = 0;
    HS->rows = 8;
    for (int i = 0; i < 8; i++) HS->counts[i] = 1 + i;
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
    strcpy((char*)PK_P(S_CAR), "viper");
    UI_G8(S_IS_VIPER) = 1;
    W.scratch = (uint8_t*)wv(0x1000);
    W.mip = (uint16_t*)wv(0x20000);
    W.bucket_color = (uint32_t*)wv(16);
    W.tga = (gxCanvas*)wv(sizeof(gxCanvas));
    W.tga->format = 5; W.tga->w = 16; W.tga->h = 8; W.tga->pitch = 64; W.tga->pixels = (uint8_t*)wv(16 * 8 * 4);
    W.tga->cx1 = 16; W.tga->cy1 = 8;
    W.tex16 = (gxCanvas*)wv(sizeof(gxCanvas));
    W.tex16->format = 4; W.tex16->w = 256; W.tex16->h = 256; W.tex16->pitch = 512; W.tex16->pixels = g_pool;
    W.tex16->cx1 = 256; W.tex16->cy1 = 256;
    W.hdr[0] = (TGAHeader*)wv(0x14);
    W.hdr[1] = (TGAHeader*)wv(0x14);
    const char* nm[6] = {"viper", "cobra", "C:\\Games\\paint.tga", "paint.tex", "x", "a_longer_car_name"};
    for (int i = 0; i < 6; i++) W.names[i] = wstr(nm[i]);
    // the paint kit's objects, by their own constructors where they have one
    W.g = (PaintKitCanvas*)wv(sizeof(PaintKitCanvas));
    g_real_canvas = &W.g->canvas;
    call_orig(F_PaintKitCanvas_ctor, 1, {U(W.g), 0, 3});
    g_real_canvas = 0;
    for (int i = 0; i < 4; i++) W.brushes[i] = UI_GP(gxCanvas, S_BRUSHES + 4 + 4 * i);
    W.brush_pixels = ((gxCanvas*)PK_P(S_BRUSH_CANVAS))->pixels;
    W.viewer = (PaintCarViewer3D*)wv(sizeof(PaintCarViewer3D));
    call_orig(F_PaintCarViewer3D_ctor, 1, {U(W.viewer), 0});
    W.cc = (ColorChooser*)wv(sizeof(ColorChooser));
    W.cc->vtbl = PK_P(VT_ColorChooser);
    W.cc->bar = stub_gxCanvasGet("colorbar.cvs");
    stub_gxAllocCanvas4(&W.cc->canvas, W.cc->bar->w, W.cc->bar->h, 5);
    W.cc->cursor = stub_gxGetStamp("c_drop.stp");
    W.ci = (ColorIndicator*)wv(sizeof(ColorIndicator));
    W.ci->vtbl = PK_P(VT_ColorIndicator);
    W.bucket = (ColorBucket*)wv(sizeof(ColorBucket));
    W.bucket->vtbl = PK_P(VT_ColorBucket);
    W.bucket->var = W.bucket_color;
    W.layers = (TemplateLayer*)wv(3 * sizeof(TemplateLayer));
    for (int i = 0; i < 3; i++) {
        call_orig(F_TemplateLayer_ctor, 1, {U(&W.layers[i]), 0});
        W.layers[i].bg = (volatile uint32_t*)PK_P(S_BG);
        W.layers[i].color = (volatile uint32_t*)PK_P(S_LAYER_COLORS + 4 * i);
        W.layers[i].frame = i;
    }
    W.preview = (TemplatePreview*)wv(sizeof(TemplatePreview));
    W.preview->vtbl = PK_P(VT_TemplatePreview);
    W.preview->bucket.vtbl = PK_P(VT_ColorBucket);
    W.preview->bucket.var = (volatile uint32_t*)PK_P(S_BG);
    W.preview->layers = W.layers;
    W.preview->count = 3;
    W.preview->bg = (volatile uint32_t*)PK_P(S_BG);
    W.preview->overlay = stub_gxCanvasGet("shade.cvs");
    W.decal = (DecalViewer*)wv(sizeof(DecalViewer));
    W.decal->vtbl = PK_P(VT_DecalViewer);
    W.decal->count = 8;
    W.decal->sel = -1;
    for (int i = 0; i < 8; i++) {
        char n[32];
        sprintf(n, "decals%d.cvs", i);
        DecalSet* s = &W.decal->sets[i];
        s->canvas = stub_gxCanvasGet(n);
        s->count = 1 + i;
        s->w = s->canvas->w;
        s->h = s->canvas->h / s->count;
        sprintf(n, "Paintkit:DecalSet:Set%d", i);
        s->name = wstr(n);
    }
    UI_GP(DecalViewer, S_DECAL_VIEWER) = W.decal;
    UI_GP(PaintKitCanvas, S_CANVAS) = W.g;
    for (int i = 0; i < 32; i++) {
        char n[32];
        sprintf(n, "templt%d.stp", i);
        W.stamps[i] = stub_gxGetStamp(n);
    }
    // a full-screen window with every control in a CustomWidget
    UIDialogItem* items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 16);
    UIDialogItem* it = items;
    it = it_(it, 23, 0, 0x131, 0x6e, 0x100, 0x100, "", 0, W.g, 0);
    it = it_(it, 23, 0, 0xf, 0x6e, 0x100, 0x100, "", 0, W.viewer, 0);
    it = it_(it, 23, 0, 0x249, 0x6e, 0x32, 0xa2, "", 0, W.cc, 0);
    it = it_(it, 23, 0, 0x252, 0x4b, 0x1e, 0x1e, "", 0, W.ci, 0);
    it = it_(it, 23, 0, 0xa9, 0x82, 0x19, 0x19, "", 0, W.bucket, 0);
    for (int i = 0; i < 3; i++) it = it_(it, 23, 0, 0x50, 0xba + 0x42 * i, 0x40, 0x40, "", 0, &W.layers[i], 0);
    it = it_(it, 23, 0, 0x131, 0x6e, 0x100, 0x100, "", 0, W.preview, 0);
    it = it_(it, 23, 0, 0x14, 0x32, 0x10e, 0x5a, "", 0, W.decal, 0);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    call_orig(uit::F_WidgetCreateWindow, 0, {0, 0, 0, 640, 480, 2});
    W.win = (WidgetWindow*)(uintptr_t)g_setup_ret;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    call_orig(uit::F_UIAddItems, 0, {U(W.win), U(items)});
    // their own Create (the originals; Added ran as each CustomWidget was added)
    const void* ctl[] = {W.g, W.cc, W.ci, W.bucket, &W.layers[0], &W.layers[1], &W.layers[2], W.preview, W.decal};
    for (const void* c : ctl) call_orig((*(const uint32_t* const*)c)[1], 1, {U(c), 0});
    for (int i = 0; i < W.win->count; i++) W.widgets.push_back((Widget*)W.win->widgets[i]);
    if (getenv("VP_DEBUG_WORLD")) {
        printf("  canvas %p (pixels %p), viewer %p, chooser %p, layers %p, preview %p, decals %p; %d widgets\n", (void*)W.g,
               (void*)W.g->canvas.pixels, (void*)W.viewer, (void*)W.cc, (void*)W.layers, (void*)W.preview, (void*)W.decal, W.win->count);
        for (int i = 0; i < W.win->count; i++)
            printf("  widget %p vtable %08x\n", (void*)W.win->widgets[i], (uint32_t)(uintptr_t)W.win->widgets[i]->vtbl);
    }
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
// the function-local Xlators: {guard, bit, Xlator, key}
static const uint32_t k_xl[][4] = {
    {0x005d43e4, 0x01, 0x005d4460, 0x00500a24}, {0x005d43e4, 0x02, 0x005d44d0, 0x00500a34}, {0x005d43e4, 0x04, 0x005d4310, 0x00500a3c},
    {0x005d43e4, 0x08, 0x005d4280, 0x00500a50}, {0x005d43e4, 0x10, 0x005d4338, 0x00500a64}, {0x005d43e4, 0x20, 0x005d4368, 0x00500a7c},
    {0x005d44dc, 0x01, 0x005d4440, 0x00500b64}, {0x005d44dc, 0x02, 0x005d4290, 0x00500b78},
    {0x005d4330, 0x01, 0x005d4358, 0x00500b94}, {0x005d4330, 0x02, 0x005d42a8, 0x00500bac},
    {0x005d4420, 0x01, 0x005d43d8, 0x00500bc0}, {0x005d4420, 0x02, 0x005d4270, 0x00500bc8}, {0x005d4420, 0x04, 0x005d4480, 0x00500bd0},
    {0x005d4420, 0x08, 0x005d42b8, 0x00500be0},
    {0x005d4454, 0x01, 0x005d4320, 0x00500c30}, {0x005d4454, 0x02, 0x005d42c8, 0x00500c40}, {0x005d4454, 0x04, 0x005d4410, 0x00500c48},
    {0x005d5ddc, 0x01, 0x005d5df0, 0x00500e84}, {0x005d5ddc, 0x02, 0x005d5de0, 0x00500e6c},
    {0x005d5dbc, 0x01, 0x005d5dd0, 0x00500eac}, {0x005d5dbc, 0x02, 0x005d5dc0, 0x00500e94},
};
static int32_t coord(int32_t lo, int32_t hi) { return chance(90) ? irange(lo, hi) : (chance(50) ? irange(-100000, 100000) : (int32_t)rnd()); }
static void random_mode_points() {
    PaintKitCanvas* g = W.g;
    g->pencil.x = coord(-10, 270); g->pencil.y = coord(-10, 270); g->pencil.color = rnd();
    g->brush.x = coord(-10, 270); g->brush.y = coord(-10, 270); g->brush.color = rnd();
    for (ShapeMode* s : {&g->line, &g->rect, &g->triangle, &g->selrect}) {
        s->x0 = coord(-10, 270); s->y0 = coord(-10, 270);
        if (chance(20)) { s->x1 = s->x0; s->y1 = s->y0; }
        else { s->x1 = coord(-10, 270); s->y1 = coord(-10, 270); }
    }
}
static void randomize_world() {
    PaintKitCanvas* g = W.g;
    g->zoom = chance(85) ? irange(1, 5) : irange(0, 6);
    g->dirty = (uint8_t)(rnd() & 1);
    g->dx0 = coord(-5, 260); g->dy0 = coord(-5, 260); g->dx1 = coord(-5, 260); g->dy1 = coord(-5, 260);
    g->mx = coord(-10, 270); g->my = coord(-10, 270);
    g->down = (uint8_t)(rnd() & 1); g->over = (uint8_t)(rnd() & 1); g->modified = (uint8_t)(rnd() & 1);
    g->paint = irange(-1, 9);
    if (chance(5)) g->w = 0;
    random_mode_points();
    PaintCarViewer3D* v = W.viewer;
    v->yaw = chance(15) ? wildf() : range(-3.5f, 3.5f);
    v->pitch = chance(15) ? wildf() : range(-1.8f, 1.8f);
    v->mx = coord(0, 640); v->my = coord(0, 480);
    v->drag = (uint8_t)(chance(80));
    for (int i = 0; i < 9; i++) v->body.m[i] = fval(-2, 2);
    for (int i = 0; i < 3; i++) v->body.p[i] = fval(-5, 5);
    *W.bucket_color = rnd();
    for (int i = 0; i < 3; i++) W.layers[i].frame = chance(85) ? irange(0, 5) : irange(-2, 8);
    W.preview->count = chance(85) ? 3 : irange(0, 3);
    DecalViewer* d = W.decal;
    d->count = chance(85) ? 8 : irange(1, 8);
    d->sel = chance(70) ? irange(-1, 12) : (int32_t)rnd();
    d->drag = (uint8_t)(chance(70));
    d->axis.total = irange(0, 10); d->axis.visible = irange(0, 5); d->axis.pos = chance(80) ? irange(0, 5) : irange(-5, 20);
    for (int i = 0; i < 8; i++) {
        DecalSet* s = &d->sets[i];
        s->count = chance(95) ? irange(1, 12) : 0;
        s->w = chance(97) ? irange(8, 64) : 0;
        s->h = chance(97) ? irange(8, 64) : 0;
        s->cols = chance(95) ? irange(1, 16) : 0;
        s->rows = irange(0, 12);
    }
    // the stubs' world
    HS->time = (int32_t)rnd();
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->grab_fail_at = chance(20) ? irange(1, 6) : -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->script_n = HS->script_pos = 0;
    HS->res_mask = rnd();
    HS->scan = chance(30);
    HS->tex_ok = chance(85);
    HS->file_ok = chance(85);
    HS->writes = 0; HS->write_fail_at = chance(25) ? irange(1, 300) : -1;
    HS->reads = 0; HS->read_fail_at = chance(25) ? irange(1, 300) : -1;
    HS->hdr_w = chance(70) ? 256 : chance(50) ? 16 : irange(0, 300);
    HS->hdr_h = chance(70) ? 256 : chance(50) ? 8 : irange(0, 300);
    HS->hdr_bad = chance(60) ? 0 : irange(1, 6);
    HS->canvas_read_ok = chance(50);
    HS->canvas_write_ok = chance(80);
    HS->rows = chance(80) ? 8 : chance(90) ? irange(1, 8) : irange(-2, 0);
    for (int i = 0; i < 8; i++) HS->counts[i] = chance(99) ? irange(1, 9) : 0;
    HS->model_next = 0; HS->tex_next = 0;
    HS->pix_seed = rnd();
    g_xl_seed = chance(80) ? 0 : rnd();
}
// .data after poisoning, and each round: the statics the code follows and the function-local state
static void randomize_statics() {
    // the ui library's
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_GU32(S_DIALOG_IDLE) = 0;
    UI_G8(S_EXIT) = 0;
    UI_GF(S_UI_DT) = chance(10) ? wildf() : range(0.0f, 0.25f);
    UI_GP(void, S_CURSOR) = 0;
    UI_G32(S_MOUSE_X) = irange(0, 639); UI_G32(S_MOUSE_Y) = irange(0, 479);
    UI_G32(S_SCREEN_W) = 640; UI_G32(S_SCREEN_H) = 480;
    UI_GP(char, 0x00504340) = W.carlist;
    UI_G32(0x00504344) = 6;
    // the paint kit's
    {
        const char* car = chance(50) ? "viper" : chance(70) ? k_cars[rnd() % 6] : "a_car_of_twenty_char";
        strcpy((char*)PK_P(S_CAR), car);
    }
    UI_G8(S_IS_VIPER) = (uint8_t)(chance(50) ? 1 : 0);
    UI_GP(PaintKitCanvas, S_CANVAS) = W.g;
    UI_GU32(S_BRUSHES) = S_BRUSH_CANVAS;
    for (int i = 0; i < 4; i++) UI_GP(gxCanvas, S_BRUSHES + 4 + 4 * i) = W.brushes[i];
    {
        gxCanvas* b = (gxCanvas*)PK_P(S_BRUSH_CANVAS);
        const int32_t w = chance(10) ? 0 : chance(10) ? 256 : irange(1, 64), h = chance(10) ? 0 : chance(10) ? 256 : irange(1, 64);
        b->format = 5; b->flags = 0; b->pixels = W.brush_pixels; b->w = w; b->h = h; b->pitch = w * 4;
        b->cx0 = 0; b->cy0 = 0; b->cx1 = w; b->cy1 = h;
    }
    UI_G32(S_TOOL) = chance(94) ? irange(0, 8) : chance(50) ? 9 : -1;
    UI_G32(S_SAVED_TOOL) = irange(0, 8);
    UI_G32(S_BRUSH) = chance(97) ? irange(0, 4) : 5;
    UI_GU32(S_LEFT) = rnd(); UI_GU32(S_RIGHT) = rnd(); UI_GU32(S_HUE) = rnd(); UI_G32(S_HUE_Y) = irange(-2, 160);
    UI_GU32(S_BG) = rnd();
    for (int i = 0; i < 3; i++) UI_GU32(S_LAYER_COLORS + 4 * i) = rnd();
    for (uint32_t a : {S_VAXIS, S_HAXIS}) {
        UI_G32(a) = irange(0, 1300); UI_G32(a + 4) = irange(0, 300); UI_G32(a + 8) = chance(90) ? irange(0, 1100) : irange(-50, 3000);
    }
    UI_G32(S_ZOOM_CB) = irange(0, 5);
    UI_G32(S_TEXTURE) = 0x70;
    UI_G32(S_MODEL) = 0x300;
    UI_G32(S_NTEMPLATES) = chance(90) ? irange(1, 32) : 0;
    UI_G32(S_TEMPLATE) = chance(90) ? irange(0, UI_G32(S_NTEMPLATES) > 0 ? UI_G32(S_NTEMPLATES) - 1 : 0) : irange(-1, 33);
    for (int i = 0; i < 32; i++) UI_GP(void, S_TEMPLATES + 4 * i) = W.stamps[i];
    UI_GP(DecalViewer, S_DECAL_VIEWER) = W.decal;
    {
        const int32_t n = HS->rows < W.decal->count ? HS->rows : W.decal->count;
        UI_G32(S_DECAL_SET) = chance(97) ? irange(0, n > 0 ? n - 1 : 0) : irange(-1, 8);
    }
    for (int i = 0; i < 12; i++) UI_GF(S_CAMERA + 4 * i) = fval(-2, 2);
    for (auto& x : k_xl) {                              // each function-local Xlator built (fresh or stale) or not
        if (chance(75)) {
            UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) | x[1]);
            UI_GU32(x[2]) = x[3];
            UI_GU32(x[2] + 4) = U(xl_text(x[3]));
            UI_GU32(x[2] + 8) = chance(70) ? UI_GU32(S_XLATOR_COOKIE) : ~UI_GU32(S_XLATOR_COOKIE);
        } else {
            UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) & ~x[1]);
        }
    }
    UI_G8(S_TEMPLATE_ONCE) = (uint8_t)((UI_G8(S_TEMPLATE_ONCE) & ~0x30) | (rnd() & 0x30));
}
// a random input script (for the functions that run a dialog)
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x1a, 0x7a, 0x13, 9, 0x18, 0x123, 0x125, 'q', '5'};
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
            const bool right = chance(25);
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
        {0x004f4208, 0x004f4210},             // TEX_TYPE, TEX_VER
        {0x004f665c, 0x004f6698},             // the ui library's Xlators' keys
        {0x004f6b78, 0x004f7228}, {0x004f7230, 0x004f7300},   // ui / _widget / widget / uistyle strings
        {0x004efbf8, 0x004efbfc},             // the current canvas
        {0x004fefb0, 0x004fefbc},             // "cross.stp"
        {0x005009a4, 0x005010b0},             // the paint kit's and tga.obj's strings
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
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static uint32_t b8(uint32_t v) { return chance(50) ? (rnd() & ~0xffu) | (v & 0xff) : v; }
static uint32_t canvas_arg() { return chance(70) ? U(W.screen) : U(W.canvas2); }
static uint32_t xy(int32_t base, int32_t span) { return (uint32_t)(chance(85) ? base + irange(-10, span + 10) : coord(-500, 1500)); }
// the tool of a class: the canvas's embedded one
static PkMode* mode_of(const char* cls) {
    PaintKitCanvas* g = W.g;
    if (!strcmp(cls, "PencilMode")) return (PkMode*)&g->pencil;
    if (!strcmp(cls, "BrushMode")) return (PkMode*)&g->brush;
    if (!strcmp(cls, "LineMode")) return (PkMode*)&g->line;
    if (!strcmp(cls, "RectMode")) return (PkMode*)&g->rect;
    if (!strcmp(cls, "TriangleMode")) return (PkMode*)&g->triangle;
    if (!strcmp(cls, "SelRectMode")) return (PkMode*)&g->selrect;
    if (!strcmp(cls, "EyeDropMode")) return (PkMode*)&g->eyedrop;
    if (!strcmp(cls, "ZoomMode")) return (PkMode*)&g->zoom_mode;
    if (!strcmp(cls, "ShapeMode")) {
        ShapeMode* s[] = {&g->line, &g->rect, &g->triangle, &g->selrect};
        return (PkMode*)s[rnd() & 3];
    }
    PkMode* all[] = {(PkMode*)&g->pencil, (PkMode*)&g->brush, (PkMode*)&g->line, (PkMode*)&g->rect, (PkMode*)&g->triangle,
                     (PkMode*)&g->selrect, (PkMode*)&g->eyedrop, (PkMode*)&g->zoom_mode};
    return all[rnd() % 8];
}
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const char* nm = f.name;
    char cls[64] = "";
    const char* meth = nm;
    if (const char* sep = strstr(nm, "::")) {
        const size_t n = (size_t)(sep - nm) < 63 ? (size_t)(sep - nm) : 63;
        memcpy(cls, nm, n);
        cls[n] = 0;
        meth = sep + 2;
    }
    uint32_t* a = f.fast ? w + 2 : w;
    memset(W.scratch, 0xa5, 0x1000);
    const bool ctor = cls[0] && !strcmp(meth, cls);
#define IS(s) (!strcmp(nm, s))
#define M(s) (!strcmp(meth, s))
    if (strstr(meth, "deleting destructor")) a[0] = rnd() & 3;
    if (strstr(nm, "Mode")) {
        w[0] = U(mode_of(cls));
        if (ctor) w[0] = U(W.scratch);
        else if (M("DrawShape")) {
            for (int i = 0; i < 4; i++) a[i] = xy(0, 256);
            if (chance(15)) a[2] = a[0];
            if (chance(15)) a[3] = a[1];
            a[4] = chance(90) ? (uint32_t)irange(0, 2) : (uint32_t)irange(3, 5);
        } else if (M("Dirty")) { for (int i = 0; i < 4; i++) a[i] = xy(0, 256); }
        else if (!strstr(meth, "deleting")) {
            a[0] = xy(0, 256);
            a[1] = xy(0, 256);
            // a third of the time the tool's own last point (the drags' "hasn't moved" cases)
            const uint32_t vt = *(const uint32_t*)(uintptr_t)w[0];
            if (chance(35) && vt != VT_EyeDropMode && vt != VT_ZoomMode) {
                const int32_t* p = (const int32_t*)(uintptr_t)w[0];
                const int k = vt == VT_BrushMode ? 2 : 3;        // BrushMode: x, y at +8; Pencil x, y and a shape's x1, y1 at +0xc
                a[0] = (uint32_t)p[k];
                a[1] = (uint32_t)p[k + 1];
            }
        }
    } else if (!strcmp(cls, "PaintKitCanvas")) {
        w[0] = U(W.g);
        if (ctor) { w[0] = U(W.scratch); a[0] = (uint32_t)irange(-1, 9); }
        else if (M("Draw")) a[0] = canvas_arg();
        else if (M("DirtyRect")) { for (int i = 0; i < 4; i++) a[i] = xy(0, 256); }
        else if (M("Callback")) { a[0] = rnd() & 3; a[1] = U(PK_P(S_VAXIS)); }
        else if (strstr(meth, "Mouse")) { a[0] = xy(W.g->x, W.g->w); a[1] = xy(W.g->y, W.g->h); }
    } else if (!strcmp(cls, "PaintCarViewer3D")) {
        w[0] = U(W.viewer);
        if (ctor) w[0] = U(W.scratch);
        else if (M("Draw")) a[0] = canvas_arg();
        else if (strstr(meth, "Mouse")) { a[0] = xy(W.viewer->mx, 40); a[1] = xy(W.viewer->my, 40); }
    } else if (!strcmp(cls, "ColorChooser")) {
        w[0] = U(W.cc);
        if (M("Draw")) a[0] = canvas_arg();
        else if (M("Callback")) { a[0] = 0; a[1] = U(PK_P(S_HUE)); }
        else if (strstr(meth, "Mouse")) { a[0] = xy(W.cc->x, 50); a[1] = xy(W.cc->y, 162); }
    } else if (!strcmp(cls, "ColorIndicator")) {
        w[0] = U(W.ci);
        if (M("Draw")) a[0] = canvas_arg();
        else if (M("Callback")) { a[0] = 0; a[1] = U(PK_P(S_LEFT)); }
    } else if (!strcmp(cls, "ColorBucket")) {
        w[0] = chance(50) ? U(W.bucket) : U(&W.layers[rnd() % 3].bucket);
        if (M("Draw")) a[0] = canvas_arg();
    } else if (!strcmp(cls, "TemplateLayer")) {
        w[0] = U(&W.layers[rnd() % 3]);
        if (ctor) w[0] = U(W.scratch);
        else if (M("Draw")) a[0] = canvas_arg();
    } else if (!strcmp(cls, "TemplatePreview")) {
        w[0] = U(W.preview);
        if (M("Draw")) a[0] = canvas_arg();
    } else if (!strcmp(cls, "DecalViewer")) {
        w[0] = U(W.decal);
        if (M("Draw")) a[0] = canvas_arg();
        else if (strstr(meth, "Mouse")) { a[0] = xy(W.decal->x, W.decal->w); a[1] = xy(W.decal->y, W.decal->h); }
        else if (M("NextSet") || M("PrevSet")) a[0] = rnd();
    } else if (IS("PaintKitDo") || IS("paint_begin") || IS("paint_end")) {
        a[0] = chance(60) ? U(W.names[0]) : U(W.names[1 + rnd() % 2 * 4]);
        a[1] = (uint32_t)irange(-1, 9);
    } else if (IS("paint_main")) a[0] = (uint32_t)irange(-1, 9);
    else if (IS("color_brush")) {
        const uint32_t c[] = {S_BRUSH_CANVAS, U(W.brushes[0]), U(W.brushes[1]), U(W.brushes[2]), U(W.brushes[3]), U(W.screen)};
        a[0] = c[rnd() % 6];
        a[1] = rnd();
    } else if (IS("make_mip")) {
        a[0] = U(W.mip);
        a[1] = chance(80) ? U(W.tex16) : U(&W.g->canvas);
        const int32_t n[] = {256, 128, 64, 32, 16, 8, 4, 2, 1, 0, -1, 3, 100, 200};
        a[2] = (uint32_t)n[rnd() % 14];
    } else if (IS("write_tex")) { a[0] = chance(80) ? U(W.tex16) : U(&W.g->canvas); a[1] = U(W.names[3]); }
    else if (IS("WriteTGA") || IS("ReadTGA")) { a[0] = chance(60) ? U(W.tga) : U(&W.g->canvas); a[1] = U(W.names[2]); }
    else if (IS("compare_headers")) {
        for (int k = 0; k < 2; k++) {
            uint8_t* h = (uint8_t*)W.hdr[k];
            memset(h, 0, 0x14);
            h[2] = 2; *(uint16_t*)(h + 0xc) = 256; *(uint16_t*)(h + 0xe) = 256; h[0x10] = 0x18; h[0x11] = 8;
        }
        if (chance(70)) {
            uint8_t* h = (uint8_t*)W.hdr[rnd() & 1];
            const int k = irange(0, 0x11);
            h[k] = (uint8_t)rnd();
        }
        a[0] = U(W.hdr[0]); a[1] = U(W.hdr[1]);
    } else a[0] = (uint32_t)irange(-3, 3);               // the button callbacks (an id), update_template_name, the installer
#undef IS
#undef M
    return true;
}
static bool is_modal(const char* nm) {
    static const char* const m[] = {"paint_main", "PaintKitDo", "exit_cb", "default_cb", "template_cb", "decal_cb", "export_cb", "import_cb"};
    for (const char* s : m)
        if (!strcmp(nm, s)) return true;
    return false;
}

#if PAINT_FIXES
// ---- the fix build: rounds kept out of the comparison ------------------------------------------------------------------------
// s's length, looking at most max + 1 characters; ~0 if it can't be read to its end (or that far)
static uint32_t fx_len(const char* s, uint32_t max) {
    for (uint32_t i = 0; i <= max; i++) {
        if (!readable(s + i, 1)) return ~0u;
        if (!s[i]) return i;
    }
    return max + 1;
}
static uint32_t fx_dec_len(int32_t v) { char t[16]; return (uint32_t)sprintf(t, "%d", v); }
// (before the original runs) a round that reaches a fixed case, where the original would overrun (or the fix gives up)
static bool fx_pre_case(const Ent& f, const uint32_t* w) {
    const char* nm = f.name;
    const uint32_t* a = f.fast ? w + 2 : w;
    const uint32_t ud = fx_len(g_user_dir, 0x103), car = fx_len(PK_CP(S_CAR), 0x103);
    const bool viper = UI_G8(S_IS_VIPER) != 0;
    auto path_long = [&](uint32_t tail) { return ud == ~0u || ud + tail > 0x103; };
    if (!strcmp(nm, "PaintKitDo") || !strcmp(nm, "paint_begin")) return fx_len((const char*)(uintptr_t)a[0], 0x1f) > 0x1f;
    if (!strcmp(nm, "PaintKitCanvas::Default") || !strcmp(nm, "default_cb")) return !viper && car > 0x1b;
    if (!strcmp(nm, "PaintKitCanvas::PaintKitCanvas"))
        return viper ? path_long(15 + fx_dec_len((int32_t)a[0])) : car == ~0u || path_long(10 + car);
    if (!strcmp(nm, "save_cb") || !strcmp(nm, "exit_cb")) {
        const PaintKitCanvas* g = UI_GP(PaintKitCanvas, S_CANVAS);
        const int32_t paint = g && readable(g, sizeof *g) ? g->paint : 0;
        return path_long(5) || (viper ? path_long(15 + fx_dec_len(paint)) : car == ~0u || path_long(10 + car));
    }
    if (!strcmp(nm, "export_cb") || !strcmp(nm, "import_cb")) return path_long(15);
    if (!strcmp(nm, "decal_cb")) return g_fx_entry1 && fx_len(g_fx_entry1, 0xed) > 0xed;   // (the stub's names: "Set<n>")
    if (!strcmp(nm, "PaintKitInstallPaintJobs")) {
        if (path_long(10)) return true;
        const char* list = UI_GP(const char, 0x00504340);
        const int32_t n = UI_G32(0x00504344);
        for (int32_t k = 0; k < n && k < 64; k++) {
            const uint32_t c = fx_len(list + 32 * k, 0x103);
            if (c == ~0u || c > 0x1a || path_long(4 + c)) return true;
        }
        return false;
    }
    if (!strcmp(nm, "DecalViewer::NextSet") || !strcmp(nm, "DecalViewer::PrevSet") || !strcmp(nm, "DecalViewer::Create")) {
        const DecalViewer* v = !strcmp(nm, "DecalViewer::Create") ? (const DecalViewer*)(uintptr_t)w[0] : UI_GP(DecalViewer, S_DECAL_VIEWER);
        if (!v || !readable(v, sizeof *v)) return false;
        int32_t k = UI_G32(S_DECAL_SET);
        if (!strcmp(nm, "DecalViewer::NextSet")) { k = (int32_t)((uint32_t)k + 1); if (!(v->count > k)) k = 0; }
        if (!strcmp(nm, "DecalViewer::PrevSet")) { k = (int32_t)((uint32_t)k - 1); if (k < 0) k = (int32_t)((uint32_t)v->count - 1); }
        const DecalSet* s = &v->sets[k];
        if (!readable(s, sizeof *s)) return false;                   // (both fault reading the set)
        const char* name = s->name;
        if (!readable(name, 1)) return false;                        // (both fault on its first byte)
        return fx_len(name, 0xff) > 0xff;
    }
    return false;
}
static bool fx_clean(const Ent& f, const Result& r) {
    return !r.fault && r.pops == (f.fast ? 4u * (uint32_t)f.nstack : 0u) && r.regs[0] == 0x0b0b0b0b && r.regs[1] == 0x05050505 &&
           r.regs[2] == 0x0d0d0d0d && r.regs[3] == 0x0e0e0e0e;
}

// ---- the fixes, each on its bad case (directed_fix_tests) -----------------------------------------------------------------
// The rewrite alone (the original would overrun there -- this program's stack or the paint kit's statics among what it would
// take), from the pristine world with the case set up: it must return cleanly (no fault, the bytes popped, ebx / esi / edi /
// ebp kept), write nothing outside what it may (every other byte of .data/.bss/.idata and the arena compared with before)
// and give what the fix promises. Each fix's boundary case (the longest input that fits) runs on the original and the
// rewrite from the same state and must give the same memory, call logs and result.
static int g_fx_bad, g_fx_n, g_fx_same_n;
static const Ent& fx_fn(const char* name) {
    for (int i = 0; i < g_nfns; i++)
        if (!strcmp(g_fns[i].name, name)) return g_fns[i];
    printf("  fix test: %s isn't listed\n", name);
    fflush(stdout);
    ExitProcess(4);
}
struct Span { const void* p; uint32_t n; };
// the first byte that changed since `before` outside the spans (the stubs' state block aside); 0 if none
static uint32_t fx_outside(const Mem& before, std::initializer_list<Span> ok) {
    auto in = [&](const uint8_t* q) {
        for (const Span& sp : ok)
            if (q >= (const uint8_t*)sp.p && q < (const uint8_t*)sp.p + sp.n) return true;
        return false;
    };
    for (uint32_t i = 0; i < DATA_BYTES; i++)
        if (before.data[i] != DATA[i] && !in(DATA + i)) return 0x004e1000 + i;
    for (uint32_t i = 0; i < IDATA_BYTES; i++)
        if (before.idata[i] != IDATA[i] && !in(IDATA + i)) return 0x005d7000 + i;
    for (uint32_t i = sizeof(HState); i < ARENA_BYTES; i++)
        if (before.arena[i] != g_arena[i] && !in(g_arena + i)) return U(g_arena + i);
    return 0;
}
static Result fx_run(const Ent& f, bool rewrite, std::initializer_list<uint32_t> w) {
    uint32_t words[72] = {};
    int i = 0;
    for (uint32_t x : w) words[i++] = x;
    g_pc = _PC_53;
    return run(f, rewrite, words);
}
static void fx_check(bool ok, const char* what, const Result* r = 0, uint32_t where = 0) {
    g_fx_n++;
    if (ok) return;
    g_fx_bad++;
    printf("  FIX TEST FAILED: %s", what);
    if (r) printf(" (fault %d %08x at %08x, popped %u, ebx esi edi ebp %08x %08x %08x %08x)", r->fault, r->code, r->eip, r->pops, r->regs[0],
                  r->regs[1], r->regs[2], r->regs[3]);
    if (where) printf(" (wrote %08x)", where);
    printf("\n");
}
static bool fx_logged(uint32_t tag) {
    for (uint32_t i = 0; i < g_log.n && i < LOG_MAX; i++)
        if (g_log.w[i] == tag) return true;
    return false;
}
static const FxNote* fx_note_of(uint32_t tag, int nth = 0) {
    for (int i = 0; i < g_fx_nn; i++)
        if (g_fx_notes[i].tag == tag && nth-- == 0) return &g_fx_notes[i];
    return 0;
}
static int fx_notes(uint32_t tag) {
    int n = 0;
    for (int i = 0; i < g_fx_nn; i++) n += g_fx_notes[i].tag == tag;
    return n;
}
static bool fx_noted(uint32_t tag, const char* s) {
    for (int i = 0; i < g_fx_nn; i++)
        if (g_fx_notes[i].tag == tag && !strcmp(g_fx_notes[i].s, s)) return true;
    return false;
}
static CallLog g_fx_log;
// the original and the rewrite from the same state: the rewrite clean; the same return, memory and call logs
static void fx_same(const Ent& f, std::initializer_list<uint32_t> w, const char* what) {
    g_fx_same_n++;
    mem_save(g_snap);
    const Result ro = fx_run(f, false, w);
    mem_save(g_after);
    memcpy(&g_fx_log, &g_log, sizeof(uint32_t) * (1 + (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)));
    mem_load(g_snap);
    const Result rn = fx_run(f, true, w);
    char m[256];
    sprintf(m, "%s: a clean return", what);
    fx_check(fx_clean(f, rn), m, &rn);
    sprintf(m, "%s: the original's result, bit for bit", what);
    const uint32_t where = mem_diff(g_after);
    const bool same = !ro.fault && where == 0 && ro.ret == rn.ret && g_log.n == g_fx_log.n &&
                      !memcmp(g_log.w, g_fx_log.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
    fx_check(same, m, &ro, where);
}
// a function of the game patched to a stub for a test (the box a callback opens), put back after
struct FxPatch {
    uint32_t at;
    uint8_t old[5];
    FxPatch(uint32_t a, void* to) : at(a) { memcpy(old, (void*)(uintptr_t)a, 5); patch_jmp(a, to); }
    ~FxPatch() { memcpy((void*)(uintptr_t)at, old, 5); }
};
static uint8_t __cdecl stub_fx_yes(const char* t, const char* q, const void*) { L('YES '); LS(t); LS(q); return 1; }
static void __cdecl stub_fx_okbox(const char* t, const char* e) { L('OKBX'); LS(t); LS(e); }

static const char k_fx_ud[] = "C:\\Games\\Viper\\Config\\";
// the tests' world: pristine; the stubs answering plainly; the viper in the paint kit, the user directory as ever
static void fx_reset() {
    mem_load(g_pristine);
    strcpy(g_user_dir, k_fx_ud);
    HS->time = 0; HS->frames = 0; HS->frame_limit = 40; HS->script_n = HS->script_pos = 0;
    HS->res_mask = 0; HS->scan = 0; HS->tex_ok = 1; HS->file_ok = 1; HS->writes = 0; HS->write_fail_at = -1; HS->reads = 0;
    HS->read_fail_at = -1; HS->hdr_w = 256; HS->hdr_h = 256; HS->hdr_bad = 0; HS->canvas_read_ok = 0; HS->canvas_write_ok = 1;
    UI_GP(PaintKitCanvas, S_CANVAS) = W.g;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_GP(DecalViewer, S_DECAL_VIEWER) = W.decal;
    strcpy((char*)PK_P(S_CAR), "viper");
    UI_G8(S_IS_VIPER) = 1;
    W.g->modified = 1;
    W.g->paint = 3;
    g_fx_nn = 0;
}
// a name of n characters (k picks the buffer): letters, from `first`
static char g_fx_str[6][0x200];
static const char* fx_str(int k, int n, char first = 'a') {
    char* s = g_fx_str[k];
    for (int i = 0; i < n && i < 0x1ff; i++) s[i] = (char)(first + i % 26);
    s[n < 0x1ff ? n : 0x1ff] = 0;
    return s;
}
// a user directory of n characters ("C:\uuu...\")
static void fx_user_dir(int n) {
    strcpy(g_user_dir, "C:\\");
    for (int i = 3; i < n - 1; i++) g_user_dir[i] = 'u';
    g_user_dir[n - 1] = '\\';
    g_user_dir[n] = 0;
}
static std::string fx_cat(const char* a, const char* b, const char* c = "") { return std::string(a) + b + c; }
// does the ResourceExists stub say `name` exists, with this mask?
static bool fx_exists(const char* name, uint32_t mask) {
    static char t[0x400];                                        // (hash_str reads only memory readable() knows)
    strncpy(t, name, sizeof t - 1);
    return ((hash_str(t) ^ mask) >> 3) % 5 != 0;
}
// a mask for which each name exists (`want` true) or not
static uint32_t fx_mask(std::initializer_list<std::pair<std::string, bool>> want) {
    for (uint32_t m = 1; m < 1000000; m++) {
        bool ok = true;
        for (auto& p : want) ok &= fx_exists(p.first.c_str(), m) == p.second;
        if (ok) return m;
    }
    printf("  fix test: no resource mask found\n");
    return 0;
}
static uint8_t* heap_end() { return g_arena + HS->heap_next; }
// "Can't create " and the path's first characters: the log line a skipped path gives (within LogReport's 0x100 bytes)
static bool fx_cant_create(const FxNote* l, const std::string& path) {
    if (!l || strncmp(l->s, "Can't create ", 13) || strlen(l->s) >= 0xff) return false;
    const size_t n = strlen(l->s + 13);
    return n == (path.size() < 200 ? path.size() : 200) && !strncmp(l->s + 13, path.c_str(), n);
}

static int directed_fix_tests() {
    char m[512];
    const Span gx = {PK_P(S_GX_CANVAS), 4};
    // ---- 1. PaintKitDo: a car name over 31 characters -- the paint kit isn't opened ----
    {
        const Ent& f = fx_fn("PaintKitDo");
        for (int n : {32, 300}) {
            fx_reset();
            const char* car = fx_str(0, n);
            mem_save(g_snap);
            g_fx_rec = true;
            const Result r = fx_run(f, true, {U(car), 3});
            g_fx_rec = false;
            sprintf(m, "PaintKitDo, a car name of %d characters: a clean return", n);
            fx_check(fx_clean(f, r), m, &r);
            const uint32_t o = fx_outside(g_snap, {});
            sprintf(m, "PaintKitDo, a car name of %d characters: nothing written", n);
            fx_check(o == 0, m, 0, o);
            sprintf(m, "PaintKitDo, a car name of %d characters: the paint kit not opened (no load, no texture, no dialog)", n);
            fx_check(!fx_logged('RSML') && !fx_logged('CTEX') && !fx_logged('MRLD') && !fx_logged('TIME') && !fx_logged('SPRF'), m);
            const FxNote* l = fx_note_of('LOGR');
            sprintf(m, "PaintKitDo, a car name of %d characters: logged, within LogReport's 0x100 bytes (\"%s\")", n, l ? l->s : "");
            fx_check(l && strstr(l->s, "too long") && strstr(l->s, std::string(car, n < 40 ? n : 40).c_str()) && strlen(l->s) < 0xff, m);
        }
        fx_reset();
        HS->canvas_read_ok = 1;                                     // (the painting read: the original Default not run)
        fx_same(f, {U(fx_str(0, 31)), 3}, "PaintKitDo, a car name of 31 characters");
        fx_reset();
        fx_same(f, {U("viper"), 3}, "PaintKitDo, the viper");
    }
    // ---- 1. paint_begin: the name keeps to S_CAR's 32 bytes ----
    {
        const Ent& f = fx_fn("paint_begin");
        for (int n : {32, 33, 300}) {
            fx_reset();
            const char* car = fx_str(0, n);
            mem_save(g_snap);
            g_fx_rec = true;
            const Result r = fx_run(f, true, {U(car)});
            g_fx_rec = false;
            sprintf(m, "paint_begin, a car name of %d characters: a clean return", n);
            fx_check(fx_clean(f, r), m, &r);
            const uint32_t o = fx_outside(g_snap, {{PK_P(S_CAR), 0x20}, {PK_P(S_IS_VIPER), 1}, {PK_P(S_TEXTURE), 4}, {PK_P(S_MODEL), 4},
                                                   {PK_P(S_CAMERA), 0x30}});
            sprintf(m, "paint_begin, a car name of %d characters: nothing written past S_CAR's 32 bytes (the brush number on)", n);
            fx_check(o == 0, m, 0, o);
            const std::string cut = std::string(car, 31);
            sprintf(m, "paint_begin, a car name of %d characters: S_CAR holds its first 31, terminated; the names built from it", n);
            fx_check(!memcmp(PK_P(S_CAR), car, 31) && UI_G8(S_CAR + 31) == 0 && fx_noted('CTEX', (cut + ".tex").c_str()) &&
                         fx_noted('RSML', (cut + ".car").c_str()) && fx_noted('MRLD', (cut + "0.mod").c_str()),
                     m);
        }
        fx_reset();
        fx_same(f, {U(fx_str(0, 31))}, "paint_begin, a car name of 31 characters");
    }
    // ---- 2 / 4. PaintKitCanvas::Default and default_cb: "<car>.cvs" whole ----
    {
        const Ent& fd = fx_fn("PaintKitCanvas::Default");
        const Ent& fc = fx_fn("default_cb");
        for (int which = 0; which < 2; which++) {
            const Ent& f = which ? fc : fd;
            FxPatch yes(F_UIDoYesNoBox, (void*)&stub_fx_yes);
            for (int n : {28, 31}) {
                fx_reset();
                const char* car = fx_str(0, n);
                strcpy((char*)PK_P(S_CAR), car);
                UI_G8(S_IS_VIPER) = 0;
                const std::string cvs = fx_cat(car, ".cvs");
                HS->res_mask = fx_mask({{cvs, true}});
                uint8_t* heap = heap_end();
                mem_save(g_snap);
                g_fx_rec = true;
                const Result r = which ? fx_run(f, true, {0}) : fx_run(f, true, {U(W.g), 0});
                g_fx_rec = false;
                sprintf(m, "%s, a car name of %d characters: a clean return", f.name, n);
                fx_check(fx_clean(f, r), m, &r);
                const uint32_t o = fx_outside(g_snap, {{W.g, sizeof(PaintKitCanvas)}, {W.g->widget, sizeof(CustomWidget)}, gx,
                                                       {heap, (uint32_t)(g_arena + ARENA_BYTES - heap)}, {PK_P(S_DEFAULT_ONCE), 1},
                                                       {PK_P(0x005d4358), 0xc}, {PK_P(0x005d42a8), 0xc}});
                sprintf(m, "%s, a car name of %d characters: nothing written outside the canvas, its widget and the heap", f.name, n);
                fx_check(o == 0, m, 0, o);
                sprintf(m, "%s, a car name of %d characters: the painting looked up and fetched as \"%s\"", f.name, n, cvs.c_str());
                fx_check(fx_noted('REXI', cvs.c_str()) && fx_noted('CGET', cvs.c_str()) && W.g->modified == 1, m);
            }
            // S_CAR unterminated (a damaged static): no name, the painting cleared
            {
                fx_reset();
                memset(PK_P(S_CAR), 'x', 0x40);
                UI_G8(S_IS_VIPER) = 0;
                uint8_t* heap = heap_end();
                mem_save(g_snap);
                g_fx_rec = true;
                const Result r = which ? fx_run(f, true, {0}) : fx_run(f, true, {U(W.g), 0});
                g_fx_rec = false;
                sprintf(m, "%s, S_CAR run on for 64 bytes: a clean return", f.name);
                fx_check(fx_clean(f, r), m, &r);
                const uint32_t o = fx_outside(g_snap, {{W.g, sizeof(PaintKitCanvas)}, {W.g->widget, sizeof(CustomWidget)}, gx,
                                                       {heap, (uint32_t)(g_arena + ARENA_BYTES - heap)}, {PK_P(S_DEFAULT_ONCE), 1},
                                                       {PK_P(0x005d4358), 0xc}, {PK_P(0x005d42a8), 0xc}});
                sprintf(m, "%s, S_CAR run on for 64 bytes: nothing written outside the canvas, its widget and the heap", f.name);
                fx_check(o == 0, m, 0, o);
                sprintf(m, "%s, S_CAR run on for 64 bytes: no painting looked up, the painting cleared", f.name);
                fx_check(!fx_logged('REXI') && !fx_logged('CGET') && fx_logged('CLR ') && W.g->modified == 1, m);
            }
            for (int n : {27, 5}) {
                fx_reset();
                strcpy((char*)PK_P(S_CAR), fx_str(0, n));
                UI_G8(S_IS_VIPER) = 0;
                HS->res_mask = fx_mask({{fx_cat(fx_str(0, n), ".cvs"), true}});
                sprintf(m, "%s, a car name of %d characters", f.name, n);
                if (which) fx_same(f, {0}, m);
                else fx_same(f, {U(W.g), 0}, m);
            }
            fx_reset();
            if (which) fx_same(f, {0}, "default_cb, the viper");
            else fx_same(f, {U(W.g), 0}, "PaintKitCanvas::Default, the viper");
        }
    }
    // ---- 3 / 5. PaintKitInstallPaintJobs: "<car>.cvs" / "~<car>.tex" whole; paths too long skipped ----
    {
        const Ent& f = fx_fn("PaintKitInstallPaintJobs");
        char* list = W.carlist;
        auto set_list = [&](std::initializer_list<const char*> cars) {
            int k = 0;
            for (const char* c : cars) { memset(list + 32 * k, 0, 32); strcpy(list + 32 * k, c); k++; }
            UI_G32(0x00504344) = k;
        };
        {
            fx_reset();
            const std::string c31 = fx_str(0, 31), c27 = fx_str(1, 27, 'k');
            set_list({"viper", c31.c_str(), c27.c_str(), "cobra"});
            HS->res_mask = fx_mask({{"~paint0.tex", false}, {c31 + ".cvs", true}, {"~" + c31 + ".tex", false}, {c27 + ".cvs", true},
                                    {"~" + c27 + ".tex", false}});
            uint8_t* heap = heap_end();
            mem_save(g_snap);
            g_fx_rec = true;
            const Result r = fx_run(f, true, {});
            g_fx_rec = false;
            fx_check(fx_clean(f, r), "PaintKitInstallPaintJobs, cars of 31 and 27 characters: a clean return", &r);
            const uint32_t o = fx_outside(g_snap, {gx, {heap, (uint32_t)(g_arena + ARENA_BYTES - heap)}});
            fx_check(o == 0, "PaintKitInstallPaintJobs, cars of 31 and 27 characters: nothing written but the heap", 0, o);
            for (const std::string* c : {&c31, &c27}) {
                sprintf(m, "PaintKitInstallPaintJobs, a car of %u characters: \"<car>.cvs\" fetched, \"~<car>.tex\" looked up, the "
                           "texture written to the user directory, all whole", (unsigned)c->size());
                fx_check(fx_noted('REXI', (*c + ".cvs").c_str()) && fx_noted('CGET', (*c + ".cvs").c_str()) &&
                             fx_noted('REXI', ("~" + *c + ".tex").c_str()) && fx_noted('FCRE', fx_cat(k_fx_ud, c->c_str(), ".tex").c_str()),
                         m);
            }
        }
        {   // a car list name run on past 58 characters (a damaged list): passed over
            fx_reset();
            char* e = list + 32;
            set_list({"viper", "cobra", "gts"});
            memset(e, 'z', 64);                                        // (entries 1 and 2 run together, then "gts"'s)
            HS->res_mask = fx_mask({{"~paint0.tex", false}, {"viper.cvs", true}, {"~viper.tex", false}});
            uint8_t* heap = heap_end();
            mem_save(g_snap);
            g_fx_rec = true;
            const Result r = fx_run(f, true, {});
            g_fx_rec = false;
            fx_check(fx_clean(f, r), "PaintKitInstallPaintJobs, a car list name of 64+ characters: a clean return", &r);
            const uint32_t o = fx_outside(g_snap, {gx, {heap, (uint32_t)(g_arena + ARENA_BYTES - heap)}});
            fx_check(o == 0, "PaintKitInstallPaintJobs, a car list name of 64+ characters: nothing written but the heap", 0, o);
            bool none = true;
            for (int i = 0; i < g_fx_nn; i++) none &= strstr(g_fx_notes[i].s, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz") == 0;
            fx_check(none && fx_noted('FCRE', fx_cat(k_fx_ud, "viper", ".tex").c_str()),
                     "PaintKitInstallPaintJobs, a car list name of 64+ characters: passed over, the other cars installed");
        }
        {   // the user directory too long for the paths
            fx_reset();
            fx_user_dir(252);
            set_list({"viper", "cobra"});
            HS->res_mask = fx_mask({{"~paint0.tex", false}, {"~paint5.tex", false}, {"cobra.cvs", true}, {"~cobra.tex", false}});
            uint8_t* heap = heap_end();
            mem_save(g_snap);
            g_fx_rec = true;
            const Result r = fx_run(f, true, {});
            g_fx_rec = false;
            fx_check(fx_clean(f, r), "PaintKitInstallPaintJobs, a user directory of 252 characters: a clean return", &r);
            const uint32_t o = fx_outside(g_snap, {gx, {heap, (uint32_t)(g_arena + ARENA_BYTES - heap)}});
            fx_check(o == 0, "PaintKitInstallPaintJobs, a user directory of 252 characters: nothing written but the heap", 0, o);
            bool logs = fx_notes('LOGR') >= 3;
            for (int i = 0; i < g_fx_nn; i++)
                if (g_fx_notes[i].tag == 'LOGR' && strcmp(g_fx_notes[i].s, "Installing paint jobs..."))
                    logs &= fx_cant_create(&g_fx_notes[i], std::string(g_user_dir) + (strstr(g_fx_notes[i].s, "cobra") ? "cobra.tex" : "paint"));
            fx_check(!fx_logged('FCRE') && logs && fx_note_of('LOGR') && fx_cant_create(fx_note_of('LOGR', 1), fx_cat(g_user_dir, "paint0.tex")),
                     "PaintKitInstallPaintJobs, a user directory of 252 characters: no texture written, each logged (\"Can't create "
                     "<path>\", cut)");
            // (the log line's path: "Can't create " and the directory and "paint0.tex", cut to 200 characters)
        }
        {
            fx_reset();
            set_list({"viper", fx_str(0, 26), "cobra"});
            HS->res_mask = fx_mask({{"~paint0.tex", false}, {fx_cat(fx_str(0, 26), ".cvs"), true}, {fx_cat("~", fx_str(0, 26), ".tex"), false}});
            fx_same(f, {}, "PaintKitInstallPaintJobs, cars of 26 characters and less");
            fx_reset();
            fx_user_dir(249);
            set_list({"viper", "cobra", "gts", "zr1", "rt10"});
            HS->res_mask = fx_mask({{"~paint0.tex", false}, {"~paint8.tex", false}, {"cobra.cvs", true}, {"~cobra.tex", false}});
            fx_same(f, {}, "PaintKitInstallPaintJobs, a user directory of 249 characters (paths of 259)");
        }
    }
    // ---- 5. save_cb: paths too long for the 0x104 bytes -- the save skipped and logged ----
    {
        const Ent& f = fx_fn("save_cb");
        struct { int ud; bool viper; bool dir; } cases[] = {{250, false, true}, {258, false, false}, {250, true, true}, {258, true, false}};
        for (auto& c : cases) {
            fx_reset();
            fx_user_dir(c.ud);
            strcpy((char*)PK_P(S_CAR), "cobra");
            UI_G8(S_IS_VIPER) = c.viper ? 1 : 0;
            W.g->modified = 1;
            uint8_t* heap = heap_end();
            mem_save(g_snap);
            g_fx_rec = true;
            const Result r = fx_run(f, true, {0});
            g_fx_rec = false;
            sprintf(m, "save_cb, %s, a user directory of %d characters: a clean return", c.viper ? "the viper" : "cobra", c.ud);
            fx_check(fx_clean(f, r), m, &r);
            const uint32_t o = fx_outside(g_snap, {{W.g, sizeof(PaintKitCanvas)}, {W.g->widget, sizeof(CustomWidget)}, gx,
                                                   {heap, (uint32_t)(g_arena + ARENA_BYTES - heap)}});
            sprintf(m, "save_cb, %s, a user directory of %d characters: nothing written outside the canvas and its widget",
                    c.viper ? "the viper" : "cobra", c.ud);
            fx_check(o == 0, m, 0, o);
            const std::string path = std::string(g_user_dir) + (c.viper ? "paint\\paint3.cvs" : "paint\\cobra.cvs");
            sprintf(m, "save_cb, %s, a user directory of %d characters: the folder %s, nothing written, the painting still changed, "
                       "\"Can't create <path>\" logged",
                    c.viper ? "the viper" : "cobra", c.ud, c.dir ? "made" : "not made (too long too)");
            fx_check((c.dir ? fx_noted('FCDR', fx_cat(g_user_dir, "paint").c_str()) : !fx_logged('FCDR')) && !fx_logged('CWR ') &&
                         !fx_logged('FCRE') && W.g->modified == 1 && fx_notes('LOGR') == 1 && fx_cant_create(fx_note_of('LOGR'), path),
                     m);
        }
        for (int viper = 0; viper < 2; viper++) {
            fx_reset();
            fx_user_dir(viper ? 243 : 244);
            strcpy((char*)PK_P(S_CAR), "cobra");
            UI_G8(S_IS_VIPER) = (uint8_t)viper;
            fx_same(f, {0}, viper ? "save_cb, the viper, a path of 259 characters" : "save_cb, cobra, a path of 259 characters");
        }
    }
    // ---- 5. export_cb / import_cb: the path too long -- their error boxes ----
    {
        FxPatch box(F_UIDoOkBox, (void*)&stub_fx_okbox);
        for (int imp = 0; imp < 2; imp++) {
            const Ent& f = fx_fn(imp ? "import_cb" : "export_cb");
            fx_reset();
            fx_user_dir(250);
            W.g->modified = 0;
            uint8_t* heap = heap_end();
            mem_save(g_snap);
            g_fx_rec = true;
            const Result r = fx_run(f, true, {0});
            g_fx_rec = false;
            sprintf(m, "%s, a user directory of 250 characters: a clean return", f.name);
            fx_check(fx_clean(f, r), m, &r);
            const uint32_t o = fx_outside(g_snap, {{W.g, sizeof(PaintKitCanvas)}, gx, {heap, (uint32_t)(g_arena + ARENA_BYTES - heap)},
                                                   {PK_P(imp ? S_IMPORT_ONCE : S_EXPORT_ONCE), 1},
                                                   {PK_P(imp ? 0x005d5dd0 : 0x005d5df0), 0xc}, {PK_P(imp ? 0x005d5dc0 : 0x005d5de0), 0xc}});
            sprintf(m, "%s, a user directory of 250 characters: nothing written outside the canvas", f.name);
            fx_check(o == 0, m, 0, o);
            sprintf(m, "%s, a user directory of 250 characters: no file opened, the error box shown%s", f.name,
                    imp ? ", the painting saved for undo and marked changed" : "");
            fx_check(!fx_logged('FCRE') && !fx_logged('FOPN') && fx_logged('OKBX') && (!imp || (fx_logged('PAST') && W.g->modified == 1)), m);
            fx_reset();
            fx_user_dir(244);
            sprintf(m, "%s, a path of 259 characters", f.name);
            fx_same(f, {0}, m);
        }
    }
    // ---- 5. PaintKitCanvas::PaintKitCanvas: the painting's path too long -- the default painting ----
    {
        const Ent& f = fx_fn("PaintKitCanvas::PaintKitCanvas");
        for (int viper = 0; viper < 2; viper++) {
            fx_reset();
            fx_user_dir(250);
            strcpy((char*)PK_P(S_CAR), "cobra");
            UI_G8(S_IS_VIPER) = (uint8_t)viper;
            HS->canvas_read_ok = 1;
            memset(W.scratch, 0, 0x1000);
            uint8_t* heap = heap_end();
            mem_save(g_snap);
            g_fx_rec = true;
            const Result r = fx_run(f, true, {U(W.scratch), 0, 3});
            g_fx_rec = false;
            sprintf(m, "PaintKitCanvas::PaintKitCanvas, %s, a user directory of 250 characters: a clean return", viper ? "the viper" : "cobra");
            fx_check(fx_clean(f, r), m, &r);
            const uint32_t o = fx_outside(g_snap, {{W.scratch, 0x1000}, gx, {heap, (uint32_t)(g_arena + ARENA_BYTES - heap)},
                                                   {PK_P(0x00500960), 0x44}, {PK_P(0x005d4260), 0x1ba0}});
            sprintf(m, "PaintKitCanvas::PaintKitCanvas, %s, a user directory of 250 characters: nothing written outside the object, "
                       "the paint kit's statics and the heap", viper ? "the viper" : "cobra");
            fx_check(o == 0, m, 0, o);
            const char* cvs = viper ? "paint3.cvs" : "cobra.cvs";
            sprintf(m, "PaintKitCanvas::PaintKitCanvas, %s, a user directory of 250 characters: no painting read, the default one (%s) "
                       "looked up", viper ? "the viper" : "cobra", cvs);
            fx_check(!fx_logged('CRD ') && fx_noted('REXI', cvs), m);
        }
        for (int viper = 0; viper < 2; viper++) {
            fx_reset();
            fx_user_dir(viper ? 243 : 244);
            strcpy((char*)PK_P(S_CAR), "cobra");
            UI_G8(S_IS_VIPER) = (uint8_t)viper;
            memset(W.scratch, 0, 0x1000);
            fx_same(f, {U(W.scratch), 0, 3}, viper ? "PaintKitCanvas::PaintKitCanvas, the viper, a path of 259 characters"
                                                  : "PaintKitCanvas::PaintKitCanvas, cobra, a path of 259 characters");
        }
    }
    // ---- 6. DecalViewer: a set's name of 256 or more characters -- its first 255 ----
    {
        static char longname[0x200], fits[0x100];
        for (int i = 0; i < 0x1ff; i++) longname[i] = (char)('A' + i % 26);
        longname[0x1ff] = 0;
        memcpy(fits, longname, 0xff);
        fits[0xff] = 0;
        const char* const fns[] = {"DecalViewer::NextSet", "DecalViewer::PrevSet", "DecalViewer::Create"};
        for (const char* fn : fns) {
            const Ent& f = fx_fn(fn);
            const bool create = !strcmp(fn, "DecalViewer::Create");
            const int32_t before = !strcmp(fn, "DecalViewer::NextSet") ? 2 : !strcmp(fn, "DecalViewer::PrevSet") ? 4 : 3;
            for (const char* nm : {(const char*)longname, (const char*)fits}) {
                fx_reset();
                W.decal->count = 8;
                W.decal->sets[3].name = nm;
                UI_G32(S_DECAL_SET) = before;
                W.decal->drag = 0x5a;
                W.decal->_821[0] = 0x11; W.decal->_821[1] = 0x22; W.decal->_821[2] = 0x33;
                if (nm == fits) {
                    sprintf(m, "%s, a set name of 255 characters", fn);
                    if (create) fx_same(f, {U(W.decal), 0}, m);
                    else fx_same(f, {0}, m);
                    continue;
                }
                uint8_t* heap = heap_end();
                mem_save(g_snap);
                const Result r = create ? fx_run(f, true, {U(W.decal), 0}) : fx_run(f, true, {0});
                sprintf(m, "%s, a set name of 511 characters: a clean return", fn);
                fx_check(fx_clean(f, r), m, &r);
                const uint32_t o = fx_outside(g_snap, {{W.decal, sizeof(DecalViewer)}, {W.decal->widget, sizeof(CustomWidget)},
                                                       {PK_P(S_DECAL_SET), 4}, {heap, (uint32_t)(g_arena + ARENA_BYTES - heap)}});
                sprintf(m, "%s, a set name of 511 characters: nothing written outside the viewer", fn);
                fx_check(o == 0, m, 0, o);
                sprintf(m, "%s, a set name of 511 characters: the title holds its first 255, terminated; the drag flag and the bytes "
                           "after it untouched", fn);
                fx_check(UI_G32(S_DECAL_SET) == 3 && !memcmp(W.decal->name, longname, 0xff) && W.decal->name[0xff] == 0 &&
                             W.decal->drag == 0x5a && W.decal->_821[0] == 0x11 && W.decal->_821[1] == 0x22 && W.decal->_821[2] == 0x33,
                         m);
            }
        }
    }
    // ---- 7. decal_cb: a decals.tab name of 238 or more characters -- its key cut, no translation ----
    {
        const Ent& f = fx_fn("decal_cb");
        static char longe[0x200], fits[0x100];
        for (int i = 0; i < 0x1ff; i++) longe[i] = (char)('a' + i % 26);
        longe[0x1ff] = 0;
        memcpy(fits, longe, 0xed);
        fits[0xed] = 0;
        for (int n : {238, 511}) {
            fx_reset();
            HS->rows = 2;
            char save = longe[n];
            longe[n] = 0;
            g_fx_entry1 = longe;
            mem_save(g_snap);
            g_fx_rec = true;
            const Result r = fx_run(f, true, {0});
            g_fx_rec = false;
            g_fx_entry1 = 0;
            longe[n] = save;
            sprintf(m, "decal_cb, a decals.tab name of %d characters: a clean return", n);
            fx_check(fx_clean(f, r), m, &r);
            const std::string key = std::string("Paintkit:DecalSet:") + std::string(longe, 216);
            bool keys = fx_notes('XLTE') == 2;
            for (int i = 0; i < g_fx_nn; i++)
                if (g_fx_notes[i].tag == 'XLTE') keys &= g_fx_notes[i].s == key;
            bool within = true;                                      // (every text the game's sprintf made: in its buffer)
            for (int i = 0; i < g_fx_nn; i++)
                if (g_fx_notes[i].tag == 'SPRF') within &= strlen(g_fx_notes[i].s) < 0x100;
            sprintf(m, "decal_cb, a decals.tab name of %d characters: each set's key cut to 234 characters (the name's first 216), "
                       "nothing formatted past 255, and \"WARNING: Can't XLAT <key>\" would fit a log line", n);
            fx_check(keys && within && 20 + key.size() < 0xff, m);
        }
        fx_reset();
        HS->rows = 2;
        g_fx_entry1 = fits;
        fx_same(f, {0}, "decal_cb, a decals.tab name of 237 characters (a key of 255)");
        g_fx_entry1 = 0;
    }
    fx_reset();
    printf("fix tests: %d checks (%d run on both, compared bit for bit), %d failed\n", g_fx_n, g_fx_same_n, g_fx_bad);
    return g_fx_bad;
}
#endif

// ---- main -----------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    const int rounds = argc > 1 ? atoi(argv[1]) : 24;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    const char* only = getenv("VP_ONLY");
    char exe[MAX_PATH], inv[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_paintkit.cpp");
    if (s) *s = 0;
    sprintf(inv, "%s\\out\\inventory.csv", exe);
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
        {F_atexit, (void*)&stub_atexit}, {uit::F_PTimeNow, (void*)&stub_PTimeNow}, {uit::F_MouseGetEvent, (void*)&stub_MouseGetEvent},
        {uit::F_KeyHit, (void*)&stub_KeyHit}, {uit::F_KeyGet, (void*)&stub_KeyGet}, {uit::F_MousePeek, (void*)&stub_MousePeek},
        {uit::F_MouseClear, (void*)&stub_MouseClear}, {uit::F_KeyClear, (void*)&stub_KeyClear}, {F_ScanDown, (void*)&stub_ScanDown},
        {F_gxGetStamp, (void*)&stub_gxGetStamp}, {F_gxForgetStamp, (void*)&stub_gxForgetStamp},
        {uit::F_gxStampWidth, (void*)&stub_gxStampWidth}, {uit::F_gxStampHeight, (void*)&stub_gxStampHeight},
        {F_gxStampCount, (void*)&stub_gxStampCount}, {uit::F_gxStampHotSpot, (void*)&stub_gxStampHotSpot},
        {uit::F_gxStampHitTest, (void*)&stub_gxStampHitTest}, {uit::F_gxDrawStamp, (void*)&stub_gxDrawStamp},
        {uit::F_gxFontGet, (void*)&stub_gxFontGet}, {uit::F_gxFontForget, (void*)&stub_gxFontForget},
        {uit::F_gxFontHeight, (void*)&stub_gxFontHeight}, {uit::F_gxFontAscent, (void*)&stub_gxFontAscent},
        {uit::F_gxFontDescent, (void*)&stub_gxFontDescent}, {uit::F_gxFontStringWidth, (void*)&stub_gxFontStringWidth},
        {uit::F_gxFontStringWidthN, (void*)&stub_gxFontStringWidthN}, {uit::F_gxFontPrintN, (void*)&stub_gxFontPrintN},
        {uit::F_gxFontPrintf, (void*)&stub_gxFontPrintf}, {uit::F_gxPaletteCreate, (void*)&stub_gxPaletteCreate},
        {uit::F_gxPaletteDestroy, (void*)&stub_gxPaletteDestroy}, {uit::F_gxPaletteMakeGradient, (void*)&stub_gxPaletteMakeGradient},
        {F_gxSetCanvas, (void*)&stub_gxSetCanvas}, {F_gxClear, (void*)&stub_gxClear}, {F_gxClearNoAlpha, (void*)&stub_gxClearNoAlpha},
        {F_gxRect, (void*)&stub_gxRect}, {F_gxLine, (void*)&stub_gxLine}, {F_gxXORLine, (void*)&stub_gxXORLine},
        {F_gxBrushLine, (void*)&stub_gxBrushLine}, {F_gxPoint, (void*)&stub_gxPoint}, {F_gxTriangle, (void*)&stub_gxTriangle},
        {F_gxGetPixel, (void*)&stub_gxGetPixel}, {F_gxPaste, (void*)&stub_gxPaste}, {F_gxPasteAlpha, (void*)&stub_gxPasteAlpha},
        {F_gxPasteZoom, (void*)&stub_gxPasteZoom}, {F_gxCut, (void*)&stub_gxCut}, {F_gxFlip, (void*)&stub_gxFlip},
        {F_gxMirror, (void*)&stub_gxMirror}, {F_gxRotate90, (void*)&stub_gxRotate90}, {F_gxMakeBrush, (void*)&stub_gxMakeBrush},
        {uit::F_gxText, (void*)&stub_gxText}, {uit::F_gxTextHeight, (void*)&stub_gxTextHeight},
        {uit::F_gxAllocCanvas, (void*)&stub_gxAllocCanvas3}, {F_gxAllocCanvas, (void*)&stub_gxAllocCanvas4},
        {F_gxFreeCanvas, (void*)&stub_gxFreeCanvas}, {uit::F_gxGrabScreen, (void*)&stub_gxGrabScreen},
        {uit::F_gxReleaseScreen, (void*)&stub_gxReleaseScreen}, {uit::F_gxFlip, (void*)&stub_gxFlipScreen},
        {uit::F_mrBeginFrame, (void*)&stub_mrBeginFrame}, {uit::F_mrEndFrame, (void*)&stub_mrEndFrame},
        {F_gxCanvasGet, (void*)&stub_gxCanvasGet}, {F_gxCanvasForget, (void*)&stub_gxCanvasForget},
        {F_gxCanvasRead, (void*)&stub_gxCanvasRead}, {F_gxCanvasWrite, (void*)&stub_gxCanvasWrite},
        {F_gxCreateTexture, (void*)&stub_gxCreateTexture}, {F_gxDestroyTexture, (void*)&stub_gxDestroyTexture},
        {F_gxGrabTexture, (void*)&stub_gxGrabTexture}, {F_gxReleaseTexture, (void*)&stub_gxReleaseTexture},
        {F_mrSetView, (void*)&stub_mrSetView}, {F_mrSetProjection, (void*)&stub_mrSetProjection},
        {F_mrModelSetClipDist, (void*)&stub_mrModelSetClipDist}, {F_mrSetCamera, (void*)&stub_mrSetCamera},
        {F_mrPushState, (void*)&stub_mrPushState}, {F_mrPopState, (void*)&stub_mrPopState}, {F_mrEnable, (void*)&stub_mrEnable},
        {F_mrModelEnvMap, (void*)&stub_mrModelEnvMap}, {F_mrModelDraw, (void*)&stub_mrModelDraw},
        {F_mrModelLoad, (void*)&stub_mrModelLoad}, {F_mrModelUnload, (void*)&stub_mrModelUnload},
        {uit::F_Xlator_xlate, (void*)&stub_xlate}, {F_Xlate, (void*)&stub_Xlate}, {F_sprintf, (void*)&stub_sprintf},
        {uit::F_LocaleConvertNumeric, (void*)&stub_LocaleConvertNumeric}, {uit::F_tolower, (void*)&stub_tolower},
        {uit::F_ControlToDisplayString, (void*)&stub_ControlToDisplayString}, {uit::F_ControlUpdate, (void*)&stub_ControlUpdate},
        {uit::F_ControlDetectInit, (void*)&stub_ControlDetectInit}, {uit::F_ControlDetect, (void*)&stub_ControlDetect},
        {uit::F_FileFindFirst, (void*)&stub_FileFindFirst}, {uit::F_FileFindNext, (void*)&stub_FileFindNext},
        {uit::F_FileFindClose, (void*)&stub_FileFindClose}, {F_FileOpen, (void*)&stub_FileOpen}, {F_FileClose, (void*)&stub_FileClose},
        {F_FileCreate, (void*)&stub_FileCreate}, {F_FileWrite, (void*)&stub_FileWrite}, {F_FileReadExact, (void*)&stub_FileReadExact},
        {F_FileCreateDirectory, (void*)&stub_FileCreateDirectory}, {F_Win32GetUserDirectory, (void*)&stub_Win32GetUserDirectory},
        {uit::F_ClipboardGetText, (void*)&stub_ClipboardGetText},
        {F_ResourceExists, (void*)&stub_ResourceExists}, {F_ResourceSetMustLoad, (void*)&stub_ResourceSetMustLoad},
        {F_ResourceSetUnload, (void*)&stub_ResourceSetUnload},
        {F_StringTableGet, (void*)&stub_StringTableGet}, {F_StringTableNumRows, (void*)&stub_StringTableNumRows},
        {F_StringTableGetEntry, (void*)&stub_StringTableGetEntry}, {F_StringTableForget, (void*)&stub_StringTableForget},
        {F_stricmp, (void*)&stub_stricmp}, {F_atoi, (void*)&stub_atoi},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pool = (uint8_t*)VirtualAlloc(0, POOL_BYTES, MEM_COMMIT, PAGE_READWRITE);
    for (uint32_t i = 0; i < POOL_BYTES; i++) g_pool[i] = (uint8_t)(i * 131 + (i >> 9));
    { DWORD old; VirtualProtect(g_pool, POOL_BYTES, PAGE_READONLY, &old); }
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    build_world();
    mem_save(g_pristine);
    printf("world: %u $E initialisers run, %d widgets in the window, %u KB of heap used\n", (unsigned)g_setup_e.size(),
           (int)W.widgets.size(), (HS->heap_next - A_HEAP) / 1024);

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
    int fixed_rounds = 0, fixed_bad = 0;                           // (the fix build: rounds kept out, a fixed case)
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0, fn_fpck = 0, fn_fixed = 0;
        long long fn_words = 0;
        const int nr = is_modal(f.name) ? rounds : rounds * 10;
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            if (poisoned) poison(g_pristine);
            randomize_world();
            randomize_statics();
            random_script(is_modal(f.name) ? 24 : 4);
            uint32_t words[72];
            if (!make_args(f, words)) { skipped++; continue; }
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            mem_save(g_snap);
#if PAINT_FIXES
            if (fx_pre_case(f, words)) {                           // a fixed case: the rewrite alone, a clean return
                const Result rn = run(f, true, words);
                fixed_rounds++;
                fn_fixed++;
                if (!fx_clean(f, rn)) {
                    printf("  FIXED CASE %08x %s (round %d): the rewrite didn't return cleanly (fault %d %08x at %08x)\n", f.v10,
                           f.name, rd, rn.fault, rn.code, rn.eip);
                    fixed_bad++;
                    fn_bad = true;
                }
                continue;
            }
#endif
            const Result ro = run(f, false, words);
            const bool changed = mem_diff(g_snap) != 0;
            fn_changed |= changed;
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
                for (uint32_t i2 = next_diff(g_snap.arena, g_arena, ARENA_BYTES, sizeof(HState)); i2 < ARENA_BYTES && !out;
                     i2 = next_diff(g_snap.arena, g_arena, ARENA_BYTES, i2 + 1))
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
            mem_save(g_after);
            memcpy(&g_log_orig, &g_log, sizeof(uint32_t) * (1 + (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)));
            mem_load(g_snap);
            const Result rn = run(f, true, words);
            checks++;
            fn_checks++;
            poisoned_checks += poisoned;
            log_words += g_log_orig.n;
            fn_words += g_log_orig.n;
            if (ro.fault || rn.fault) { faults++; fn_faults++; }
            if (ro.fault && rn.fault) both_fault++;
            bool same = ro.fault == rn.fault && ro.code == rn.code && ro.ret == rn.ret && ro.pops == rn.pops && ro.top == rn.top &&
                        !memcmp(&ro.st0, &rn.st0, sizeof ro.st0) && !memcmp(ro.regs, rn.regs, sizeof ro.regs);
            const uint32_t where = mem_diff(g_after);
            same &= where == 0;
            same &= g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
            if (!same) {
                printf("  MISMATCH %08x %s (round %d, %s, pc %s)\n", f.v10, f.name, rd, poisoned ? "poisoned" : "as loaded", g_pc == _PC_24 ? "24" : "53");
                printf("    fault %d/%d (%08x at %08x / %08x at %08x), return %08x / %08x, x87 top %u / %u, popped %u / %u,"
                       " ebx esi edi ebp %08x %08x %08x %08x / %08x %08x %08x %08x\n",
                       ro.fault, rn.fault, ro.code, ro.eip, rn.code, rn.eip, ro.ret, rn.ret, ro.top, rn.top, ro.pops, rn.pops,
                       ro.regs[0], ro.regs[1], ro.regs[2], ro.regs[3], rn.regs[0], rn.regs[1], rn.regs[2], rn.regs[3]);
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
                differ++;
                fn_bad = true;
            }
            if (rd == 0) { pure_n += fp.pure; replay_n += fp.replay_only != 0; }
        }
        bad_fns += fn_bad;
        (fn_changed ? changed_fns : still_fns)++;
        if (trace)
            printf("%08x %-58s %s%s (%d checks, %d with the footprint checked, %d faulted, %lld words logged a check, %d kept out: a"
                   " fixed case)\n", f.v10, f.name, fn_bad ? "BAD" : "ok", fn_changed ? "" : " (never changed memory)", fn_checks, fn_fpck,
                   fn_faults, fn_checks ? fn_words / fn_checks : 0, fn_fixed);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    printf("%d functions (%d pure, %d replay_only in round 0, %d others), %d listed twice; %lld checks (%lld on poisoned .data, %lld "
           "with the footprint checked), %lld logged words compared: %d differ, %d footprint violations, %d checks faulted (%lld in "
           "both, the same way), %d skipped; %d functions bad\n", g_nfns, pure_n, replay_n, g_nfns - pure_n - replay_n, dup, checks,
           poisoned_checks, fp_checks, log_words, differ, fp_bad, faults, both_fault, skipped, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
#if PAINT_FIXES
    printf("the fix build: %d rounds reached a fixed case (kept out of the comparison; the rewrite returned cleanly on %d)\n",
           fixed_rounds, fixed_rounds - fixed_bad);
    const int fx_bad = directed_fix_tests();
    mem_load(g_pristine);
    return differ || fp_bad || dup || fixed_bad || fx_bad ? 1 : 0;
#else
    return differ || fp_bad || dup ? 1 : 0;
#endif
}
