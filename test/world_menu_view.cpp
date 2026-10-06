// world_menu_view.cpp -- M3 UI stage, step U2 (group C): every rewrite of hook/menu_view.cpp (carview.obj, trkview.obj,
// oppview.obj, optview.obj) and hook/menu_board.cpp (board.obj) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_menu_view.cpp
//        /Fo<dir>\ /Fe<dir>\world_menu_view.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//     (and with /DVP_MENU_FIXES: the fix build, below)
//   run:   world_menu_view.exe [rounds] [seed]          (VP_TRACE=1: one line per function; VP_ONLY=text: those only;
//          VP_DEBUG_WORLD=1: the world's objects and widgets, and every fault's registers)
//
// Loads out\race_v10.exe at 0x400000 the way test/world_ui.cpp does (a child process with the range reserved) and includes
// the two rewrite files with PORT_FN redefined to list each function (its v1.0 address, the rewrite, its calling convention,
// stack arguments and return, its footprint). VP_FAITHFUL: the rewrites exactly as the originals.
//
// The world. Once: the $E static initialisers of libraries ui and menu (their Xlators, colours) and the game's UIBegin (the
// styles); a car list for the root's GetCarFileName / GetMaxCarFileNames (the originals run) and a LocaleInfo; then every
// class built by its own original constructor -- a TrackChooser, a lone TrackViewer, a CarChooser, a lone CarViewer3D (on
// the root's car list), an OpponentViewer (its car set), a RaceOptionViewer, a HighScoreBoardTable, a BoardCustomText, a
// BoardControl (BoardCreateControl) -- put in a full-screen window by the game's WidgetCreateWindow and _UIAddItems
// (CustomWidgets: adding one runs its control's Added -- prev / next buttons, the viewers' CustomWidgets, the option
// viewer's groups), then their own Create (notifications); a RaceResultTable on its own (it's abstract: no Draw); a
// GameOptions, records, the callers' buffers. Translations are short (the stubbed Xlator::xlate gives one of a few
// abbreviations and words by the key, now and then a longer one), as the game's are. This is the pristine state.
//
// Then for EVERY function, `rounds` times (ten times that for one that doesn't run a dialog): pristine restored; half the
// rounds POISONED (.data/.bss random but for the strings and tables the code reads and what the ui library follows); then
// the world's state random: the objects' fields (tracks -1..9, cars and paint jobs past their ranges, the turn's time and
// pose, stats, NaN / infinite / huge floats, opponent types -1..3, names), the options, the records, each function-local
// Xlator built or not (a built one with its cookie fresh or stale), the details dialogs' static UIDialog set or not, the
// statics pointing at the objects; arguments made for the function (the right object, canvases, names, ids, flags); for
// the dialogs (details, BoardDo, the callbacks that run them) a random input script, ended by a watchdog. The stack filled
// with one pattern; the ORIGINAL runs from a raw call thunk; its results kept, the snapshot restored, the REWRITE runs.
// Compared: .data/.bss/.idata and the 1 MB arena (a pointer into this thread's stack in both counts as equal), the return
// value (st0 for Hermite), the x87 depth, bytes popped, ebx / esi / edi / ebp, faults, and every stub's call log. Every
// byte the original changed must lie in the rewrite's footprint (unless replay_only); a pure one changes nothing. The x87
// at 24 or 53 bits (alternating rounds).
//
// Stubbed (logged; state in the arena so both passes see the same): world_ui.cpp's set (memory, logs, input and time, the
// 2D calls -- gxFontPrintf logs its format's arguments --, Xlator::xlate, sprintf, the controls, files) and this group's
// callees outside the menus: options (OptionsGet answers from the round's table), the track table (GetTrackCount / Name /
// Number / FriendlyName / Text / Difficulty, GetLapCountFromType), the enum strings (realism, race type, ghost car type,
// weather, time of day, AI strength, field, event), resources, string tables (a car's .tab), records (RecordMgrCreate /
// Destroy / SetMode / Clear / GetNthBest*), PhysicsTimeString, LocaleFormatShortDate, the 3D renderer (mrSetView,
// mrSetProjection, mrSetCamera, mrEnable / Disable, mrModel*: every frame's 12 dwords logged), WorldGetCarTexture,
// CarFileLoad (fills the CarFile's wheel fields from the round), PaintKitDo, and the CRT's stricmp / atof / atoi (their
// locale paths). The game's own code everywhere else: the widget toolkit (UIDoDialog, _UIAddItems, the groups,
// CreateMultiString, UIStyleDraw / Width, UIDeltaT), UIDialogItem's and Xlator's constructors, the matrix functions,
// gxSetClip / gxRestoreClip, strchr / strrchr / __ftol, HackEnabled, GetCarFileNumber and the car list, and every function
// of this group (each rewrite is checked against its original with the same callees).
//
// Built with /DVP_MENU_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Menus"). Every
// function is still compared as above, with two kinds of round kept out of the comparison and counted (per function,
// listed): one where only the original faults (a crash the fixes remove: SetModel's name with no '.'), and one where the
// original's sprintf printed past the field it was given (sprintf's stub knows the fields of the world's objects: the
// car chooser's stat texts, the option viewer's summary and laps text) -- huge stats and the longer translations do that
// in ordinary rounds, and the fixed rewrite keeps to the field. sprintf's destination isn't logged (update_car formats
// in a buffer of its own now; what lands in the field is compared in memory). Then directed_fix_tests: for each fix, the
// bad case run on the rewrite alone -- no fault, the bytes popped and ebx / esi / edi / ebp kept, nothing written outside
// the object or buffer (the memory around it compared), and the result the fix promises -- and its boundary case (the
// longest input that fits) on both, compared bit for bit; and the board text's format check against the game's own
// printf (run on every test text and on random ones with its arguments on a no-access page: it faults exactly when the
// check says it reads one). Without it (VP_FAITHFUL) every rewrite must match its original bit for bit.
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
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif

#ifndef VP_MENU_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#define MENU_FIXES 0
#else
#define MENU_FIXES 1
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

#include "../hook/menu_view.cpp"
#include "../hook/menu_board.cpp"

using namespace uit;
using namespace mview;

// ---- random numbers --------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x6a09e667u;
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
enum { ARENA_BYTES = 0x100000 };
static uint8_t* g_arena;
struct Mem { uint8_t* data; uint8_t* idata; uint8_t* arena; };
static Mem g_pristine, g_snap, g_after;
static Mem mem_alloc() { return {(uint8_t*)malloc(DATA_BYTES), (uint8_t*)malloc(IDATA_BYTES), (uint8_t*)malloc(ARENA_BYTES)}; }
static void mem_save(Mem& m) { memcpy(m.data, DATA, DATA_BYTES); memcpy(m.idata, IDATA, IDATA_BYTES); memcpy(m.arena, g_arena, ARENA_BYTES); }
static void mem_load(const Mem& m) { memcpy(DATA, m.data, DATA_BYTES); memcpy(IDATA, m.idata, IDATA_BYTES); memcpy(g_arena, m.arena, ARENA_BYTES); }
static ULONG_PTR g_stack_lo, g_stack_hi;
static bool on_stack(uint32_t a) { return a >= g_stack_lo && a < g_stack_hi; }
static uint32_t block_diff(const uint8_t* saved, const uint8_t* live, uint32_t n, uint32_t i) {
    while (i < n) {
        if (!memcmp(saved + i, live + i, n - i)) return n;
        while (i < n && saved[i] == live[i]) i++;
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
    // this group's stubs
    uint32_t opt_mask;
    int32_t opt_val[8];
    int32_t lap_seed, model_next, model_zero, cf_ok, rec_mask, tab_seed, tab_null;
    float ext[6];
    float cf[10];
    char texname[16];
};
#define HS ((HState*)g_arena)
enum : uint32_t { A_HEAP = 0x60000 };

// ---- the log ----------------------------------------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 16 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void L(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static uintptr_t g_self_lo, g_self_hi;
static bool readable(const void* p, uint32_t n) {
    const uintptr_t a = (uintptr_t)p;
    if (a >= (uintptr_t)g_arena && a + n <= (uintptr_t)g_arena + ARENA_BYTES) return true;
    if (a >= 0x401000 && a + n <= 0x5d9000) return true;
    if (a >= g_self_lo && a + n <= g_self_hi) return true;
    if (on_stack((uint32_t)a) && on_stack((uint32_t)(a + n - 1))) return true;
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
static void L_canvas() {
    const gxCanvas* c = UI_GP(gxCanvas, S_GX_CANVAS);
    L(P(c));
    if (c && readable(c, sizeof(gxCanvas))) { L((uint32_t)c->cx0); L((uint32_t)c->cy0); L((uint32_t)c->cx1); L((uint32_t)c->cy1); }
}
static void L_block(const void* p, int dwords) {                  // a frame / matrix / point: its dwords (by value)
    if (!readable(p, 4u * (uint32_t)dwords)) { L('BADB'); L(P(p)); return; }
    const uint32_t* w = (const uint32_t*)p;
    for (int i = 0; i < dwords; i++) L(w[i]);
}
// the directed fix tests' hooks into the stubs (all off in the random rounds), and what the stubs last saw
static const char* g_fx_track;                        // GetTrackName's answer
static const char* g_fx_tab6;                         // a .tab's entry 6
static const char* g_fx_xl;                           // every Xlator's text (when it's refreshed)
static const char* g_fx_enum[8];                      // an enum's string
static bool g_fx_real_tex;                            // WorldGetCarTexture's names as the game's (their lengths matter)
static char g_rec_stamp[0x200], g_rec_cf[0x200], g_rec_mustload[0x200], g_rec_load[0x200], g_rec_remap_load[0x200], g_rec_fprf[0x400];
static uint8_t g_rec_remap[0x28];
static const char* g_rec_fprf_fmt;
static uint32_t g_rec_fprf_a0;
static char g_rec_mark;                               // gxFontPrintf: record only a format starting with it (0: any)
// the fix build: whether the original's sprintf printed past a field of the world's objects (a fixed case)
static bool g_overran;
static uint32_t field_size(const char* p);
static uint32_t hash_str(const char* s) {
    uint32_t h = 2166136261u;
    if (!s || !readable(s, 1)) return 0x5eed;
    for (; readable(s, 1) && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static void rec(char* dst, uint32_t n, const char* s) {
    uint32_t i = 0;
    if (s && readable(s, 1))
        for (; i + 1 < n && readable(s + i, 1) && s[i]; i++) dst[i] = s[i];
    dst[i] = 0;
}

// ---- stubs: memory, logs, sync (world_ui.cpp's) --------------------------------------------------------------------------------
static void* __cdecl stub_MemAlloc(int n) {
    L('ALOC'); L((uint32_t)n);
    uint32_t a = (HS->heap_next + 7) & ~7u;
    if (a < A_HEAP || a + (uint32_t)(n > 0 ? n : 0) > ARENA_BYTES - 0x100 || n < 0 || n > 0x40000) { L('OOM '); return 0; }
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
static void __cdecl stub_LogReport(const char* fmt, ...) { L('LOGR'); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); }
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
    *x = w->x + (g->x0 + g->x1) / 2;
    *y = w->y + (g->y0 + g->y1) / 2;
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

// ---- stubs: the 2D calls (world_ui.cpp's) -------------------------------------------------------------------------------------
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
static void* __cdecl stub_gxGetStamp(const char* name) { L('GSTP'); LS(name); rec(g_rec_stamp, sizeof g_rec_stamp, name); return fake_for(name, 'STMP'); }
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
// (with its format's arguments: a string's text, a number's dwords)
static void __cdecl stub_gxFontPrintf(const void* f, const void* pal, uint32_t flags, int32_t x, int32_t y, const char* fmt, ...) {
    L('FPRF'); L(P(f)); L(P(pal)); L(flags); L((uint32_t)x); L((uint32_t)y); LS(fmt);
    va_list ap;
    va_start(ap, fmt);
    if (!g_rec_mark || (fmt && readable(fmt, 1) && fmt[0] == g_rec_mark)) {
        g_rec_fprf_fmt = fmt;
        g_rec_fprf_a0 = *(const uint32_t*)ap;
        rec(g_rec_fprf, sizeof g_rec_fprf, fmt);
    }
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
static void __cdecl stub_gxPaste(const gxCanvas* src, int32_t x, int32_t y) { L('PAST'); L(P(src)); L((uint32_t)x); L((uint32_t)y); L_canvas(); }
static void __cdecl stub_gxText(int32_t x, int32_t y, const char* s, uint32_t col) { L('TEXT'); L((uint32_t)x); L((uint32_t)y); LS(s); L(col); L_canvas(); }
static int32_t __cdecl stub_gxTextHeight() { L('TXTH'); return 8; }
static void __cdecl stub_gxAllocCanvas(gxCanvas* c, int32_t w, int32_t h) {
    L('ACNV'); L(P(c)); L((uint32_t)w); L((uint32_t)h);
    c->format = 4; c->flags = 0; c->pixels = (uint8_t*)(uintptr_t)(0x7c000000u + (uint32_t)(HS->alloc_canvas_next++) * 0x100000u);
    c->w = w; c->h = h; c->pitch = w * 2; c->cx0 = 0; c->cy0 = 0; c->cx1 = w; c->cy1 = h;
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
static void __cdecl stub_gxFlip() { L('FLIP'); }
static void __cdecl stub_mrBeginFrame() { L('MRBF'); }
static void __cdecl stub_mrEndFrame() { L('MREF'); }

// ---- stubs: text, controls, files (world_ui.cpp's) --------------------------------------------------------------------------------
// (a translation as the game's are: short -- "s", "bhp", "Laps" --, one of 16 by the key and the round; now and then a
// longer one)
static const char* const k_xl_text[16] = {"s", "bhp", "rpm", "lb-ft", "mph", "Laps", "Pack", "Ghost", "Clock", "DNF", "Best",
                                          "Car", "Track", "Weather", "Time of day", "A longer translation"};
static uint32_t g_xl_seed;
static const char* xl_text(uint32_t key) {
    const uint32_t h = hash_str((const char*)(uintptr_t)key) ^ g_xl_seed;
    return k_xl_text[(h & 0xff) < 0xf0 ? h % 14 : 14 + (h & 1)];
}
static void __fastcall stub_xlate(uint32_t* xl, int) {
    L('XLAT'); L(P(xl));
    xl[1] = (uint32_t)(uintptr_t)(g_fx_xl ? g_fx_xl : xl_text(xl[0]));
    xl[2] = UI_GU32(S_XLATOR_COOKIE);
}
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    L('SPRF'); L(MENU_FIXES ? 'DEST' : P(buf)); LS(fmt);          // (the fix build: some format into a buffer of their own)
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
    if (MENU_FIXES) {
        const uint32_t sz = field_size(buf);
        if (sz && strlen(buf) >= sz) g_overran = true;
    }
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
static int32_t __cdecl stub_FileOpen(const char* name) { L('FOPN'); LS(name); return 0; }
static void __cdecl stub_FileClose(int32_t* fd) { L('FCLS'); L((uint32_t)*fd); *fd = 0; }
static uint8_t __cdecl stub_ClipboardGetText(char* buf, int n) { L('CLIP'); if (n > 0) buf[0] = 0; return 0; }

// ---- stubs: this group's callees outside the menus -------------------------------------------------------------------------------
static const char* const k_tracks[] = {"bemidji", "heaven", "uptown", "hastings", "limbo", "dundas", "nfield", "kenyon", "hell"};
static char g_friendly[10][24], g_text[10][24], g_enum[8][8][16], g_time[16][16], g_tabs_str[4][16][24];
static uint8_t g_tabs[4][16];                         // fake StringTables (their addresses)
static uint8_t g_mgr[0x20];                           // a fake RecordMgr
// options: the round's answer for a key, or none (the original then sets the default)
static void __cdecl stub_OptionsGet(const char* sec, const char* key, int32_t* out) {
    L('OPTG'); LS(sec); LS(key); L(P(out));
    const uint32_t h = hash_str(key) ^ HS->opt_mask;
    if (h & 1) {
        *out = HS->opt_val[(h >> 1) & 7];
        L((uint32_t)*out);
    }
}
static void __cdecl stub_OptionsSet(const char* sec, const char* key, int32_t v) { L('OPTS'); LS(sec); LS(key); L((uint32_t)v); }
static int32_t __cdecl stub_GetTrackCount() { L('TCNT'); return 8; }
static const char* __cdecl stub_GetTrackName(int32_t t) {
    L('TNAM'); L((uint32_t)t);
    if (g_fx_track) return g_fx_track;
    return t >= 0 && t <= 8 ? k_tracks[t] : "zzz";
}
static const char* __cdecl stub_GetTrackFriendlyName(int32_t t) { L('TFRN'); L((uint32_t)t); return g_friendly[(uint32_t)(t + 1) % 10]; }
static const char* __cdecl stub_GetTrackText(int32_t t) { L('TTXT'); L((uint32_t)t); return g_text[(uint32_t)(t + 1) % 10]; }
static int32_t __cdecl stub_GetTrackNumber(const char* name) {
    L('TNUM'); LS(name);
    if (!name || !readable(name, 1)) return -1;
    for (int i = 0; i < 9; i++)
        if (!_strnicmp(k_tracks[i], name, strlen(k_tracks[i]))) return i;
    return -1;
}
static int32_t __cdecl stub_GetTrackDifficulty(int32_t t) { L('TDIF'); L((uint32_t)t); return (int32_t)((uint32_t)t % 3u); }
static int32_t __cdecl stub_GetLapCountFromType(int32_t type, const char* name) {
    L('LAPS'); L((uint32_t)type); LS(name);
    return 1 + (int32_t)(((uint32_t)type + hash_str(name) + (uint32_t)HS->lap_seed) & 7u);
}
template <int K> static const char* __cdecl stub_enum_string(int32_t v) {
    L('ENUM'); L(K); L((uint32_t)v);
    return g_fx_enum[K] ? g_fx_enum[K] : g_enum[K][(uint32_t)v & 7];
}
static void __cdecl stub_ResourceSetMustLoad(const char* s) { L('RSML'); LS(s); rec(g_rec_mustload, sizeof g_rec_mustload, s); }
static void __cdecl stub_ResourceSetUnload(const char* s) { L('RSUL'); LS(s); }
static void __cdecl stub_LocaleFormatShortDate(char* buf, int32_t n, int32_t d, int32_t m, int32_t y) {
    L('DATE'); L(P(buf)); L((uint32_t)n); L((uint32_t)d); L((uint32_t)m); L((uint32_t)y);
    _snprintf(buf, (size_t)(n > 0 ? n : 0), "%d/%d/%d", d, m, y);
    if (n > 0) buf[n - 1] = 0;
}
static void* __cdecl stub_StringTableGet(const char* name) { L('STGT'); LS(name); return HS->tab_null ? 0 : g_tabs[hash_str(name) & 3]; }
static void __cdecl stub_StringTableForget(void* t) { L('STFG'); L(P(t)); }
static const char* __cdecl stub_StringTableGetEntry(const void* t, int32_t row, int32_t col) {
    L('STEN'); L(P(t)); L((uint32_t)row); L((uint32_t)col);
    uint32_t k = 0;
    for (int i = 0; i < 4; i++) if (t == g_tabs[i]) k = (uint32_t)i;
    if (g_fx_tab6 && row == 6) return g_fx_tab6;
    return g_tabs_str[k][((uint32_t)row + (uint32_t)HS->tab_seed) & 15];
}
static const void* __cdecl stub_RecordMgrCreate(const char* name) { L('RMCR'); LS(name); return g_mgr; }
static void __cdecl stub_RecordMgrDestroy(const void* m) { L('RMDS'); L(P(m)); }
static void __fastcall stub_RecordMgr_Clear(const void* m, int) { L('RMCL'); L(P(m)); }
static void __fastcall stub_RecordMgr_SetMode(const void* m, int, int32_t a, int32_t b, uint8_t c) { L('RMSM'); L(P(m)); L((uint32_t)a); L((uint32_t)b); L(c); }
static MvRaceRecord* g_recs;                          // 8, in the arena
template <int K> static const void* __fastcall stub_GetNth(const void* m, int, int32_t n) {
    L('RMGN'); L(K); L(P(m)); L((uint32_t)n);
    if (((uint32_t)HS->rec_mask >> ((uint32_t)(K * 8 + n) & 31)) & 1) return &g_recs[((uint32_t)(n + K)) & 7];
    return 0;
}
static const char* __cdecl stub_PhysicsTimeString(uint32_t bits, uint8_t h) {
    L('PTST'); L(bits); L(h);
    char* s = g_time[bits & 15];
    sprintf(s, "t%08x", bits);
    return s;
}
// the 3D renderer
static void __cdecl stub_mrSetView(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t c) { L('MRSV'); L(x); L(y); L(w); L(h); L(c); }
static void __cdecl stub_mrSetProjection(uint32_t a, uint32_t b, uint32_t c) { L('MRSP'); L(a); L(b); L(c); }
static void __cdecl stub_mrSetCamera(const void* fr) { L('MRSC'); L_block(fr, 12); }
static void __cdecl stub_mrEnable(int32_t f) { L('MREN'); L((uint32_t)f); }
static void __cdecl stub_mrDisable(int32_t f) { L('MRDI'); L((uint32_t)f); }
static void __cdecl stub_mrModelEnvMap(uint8_t on) { L('MREM'); L(on); }
static void __cdecl stub_mrModelSetAlpha(uint32_t a) { L('MRSA'); L(a); }
static void __cdecl stub_mrModelClearAlpha() { L('MRCA'); }
static void __cdecl stub_mrModelDraw(int32_t m, const void* fr) { L('MRDR'); L((uint32_t)m); L_block(fr, 12); }
static int32_t __cdecl stub_mrModelLoad(const char* name) {
    L('MRLD'); LS(name);
    rec(g_rec_load, sizeof g_rec_load, name);
    return HS->model_zero ? 0 : 0x200 + (int32_t)(hash_str(name) & 0xff);
}
static int32_t __cdecl stub_mrModelLoadRemap(const char* name, const char* remap, int32_t n) {
    L('MRLR'); LS(name); LS(remap); LS(remap + 0x10); L_block(remap + 0x20, 2); L((uint32_t)n);
    rec(g_rec_remap_load, sizeof g_rec_remap_load, name);
    if (readable(remap, 0x28)) memcpy(g_rec_remap, remap, 0x28);
    return HS->model_zero ? 0 : 0x300 + (int32_t)(hash_str(name) & 0xff);
}
static void __cdecl stub_mrModelUnload(int32_t m) { L('MRUL'); L((uint32_t)m); }
static void __cdecl stub_mrModelRemapTextures(int32_t m, const char* remap, int32_t n) {
    L('MRRT'); L((uint32_t)m); LS(remap); LS(remap + 0x10); L_block(remap + 0x20, 2); L((uint32_t)n);
    if (readable(remap, 0x28)) memcpy(g_rec_remap, remap, 0x28);
}
static int32_t __cdecl stub_mrModelCopyTransform(int32_t m, const void* mat) { L('MRCT'); L((uint32_t)m); L_block(mat, 9); return 0x400 + HS->model_next++; }
static void __cdecl stub_mrModelDestroyCopy(int32_t m) { L('MRDC'); L((uint32_t)m); }
static void __cdecl stub_mrModelGetExtents(int32_t m, float* a, float* b, float* c, float* d, float* e, float* f) {
    L('MRGE'); L((uint32_t)m);
    float* o[6] = {a, b, c, d, e, f};
    for (int i = 0; i < 6; i++) *o[i] = HS->ext[i];
}
static void __cdecl stub_WorldGetCarTexture(char* out, const char* base, int32_t n) {
    L('WGCT'); L(P(out)); LS(base); L((uint32_t)n);
    if (g_fx_real_tex) {                                            // (the game's: 0x462070)
        if (!_stricmp(base, "viper")) {
            if (n < 0) sprintf(out, "~paint%d.tex", -1 - n);
            else sprintf(out, "%s%d.tex", base, n + 1);
        } else if (n < 0) sprintf(out, "~%s.tex", base);
        else sprintf(out, "%s.tex", base);
        return;
    }
    sprintf(out, "%s%d", HS->texname, n & 0xff);
}
static uint8_t __cdecl stub_CarFileLoad(void* cf, const char* name, void* p) {
    L('CFLD'); L(P(cf)); LS(name); L(P(p));
    rec(g_rec_cf, sizeof g_rec_cf, name);
    static const uint32_t at[10] = {0x0057b9e0, 0x0057b9e4, 0x0057b9f8, 0x0057bb68, 0x0057bb6c, 0x0057bb70, 0x0057bb74, 0x0057bb78, 0x0057bb7c, 0x0057b9ec};
    for (int i = 0; i < 10; i++) UI_GF(at[i]) = HS->cf[i];
    return (uint8_t)HS->cf_ok;
}
static void __cdecl stub_PaintKitDo(const char* name, int32_t p) { L('PKIT'); LS(name); L((uint32_t)p); }
static int __cdecl stub_stricmp(const char* a, const char* b) {
    L('SICM'); LS(a); LS(b);
    if (!readable(a, 1) || !readable(b, 1)) { *(volatile int*)0 = 0; }
    const int r = _stricmp(a, b);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
static double __cdecl stub_atof(const char* s) { L('ATOF'); LS(s); return readable(s, 1) ? strtod(s, 0) : 0.0; }
static int32_t __cdecl stub_atoi(const char* s) { L('ATOI'); LS(s); return readable(s, 1) ? atoi(s) : 0; }

// ---- the raw call (world_ui.cpp's) -------------------------------------------------------------------------------------------------
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
    return EXCEPTION_EXECUTE_HANDLER;
}
__declspec(noinline) static void fill_stack() {
    volatile uint32_t buf[0x30000 / 4];
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
    fill_stack();
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    if (vp_try([&] { raw_call(); }, fault_filter)) { r.fault = 1; r.code = g_fault_code; r.eip = g_fault_eip; }
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
#else
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
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
    GameOptions* opts;
    TrackChooser* tc;
    TrackViewer* tv;
    CarChooser* cc;
    CarViewer3D* cv;
    OpponentViewer* ov;
    RaceOptionViewer* rov;
    RaceResultTable* rrt;
    HighScoreBoardTable* hs;
    BoardCustomText* bct;
    BoardControl* bc;
    char* track_name;                                 // the choosers' callers' buffers
    char* car_name;
    char* summary;
    uint8_t* flags;                                   // [0] the track chooser's not-test flag, [1] the option viewer's flag
    int32_t* ints;                                    // [0] the opponent viewer's out, [1] ai count, [2] out2
    char* carlist;                                    // 32-byte names for the root's car list
    uint8_t* locale;
    char* texts[8];                                   // random strings (0x40 each)
    uint8_t* scratch;                                 // fresh blocks for constructors (0x400)
    UIDialogItem* static_items;                       // a list for a details dialog's static UIDialog set earlier
    std::vector<Widget*> widgets;
};
static World W;
static std::vector<uint32_t> g_setup_e;
// the fields of the world's objects the rewrites format into (their sizes): the car chooser's stat texts, the option
// viewer's summary (MenuDoRaceSetup's is 0x20 bytes) and laps text
static uint32_t field_size(const char* p) {
    if (W.cc) {
        CarChooser* c = W.cc;
        const char* f16[] = {c->s18, c->s28, c->s38, c->s48, c->s58, c->s88, c->sd8};
        for (const char* q : f16) if (p == q) return 0x10;
        if (p == c->s98 || p == c->sb8) return 0x20;
    }
    if (p == W.summary) return 0x20;
    if (W.rov && p == W.rov->text) return sizeof W.rov->text;
    return 0;
}

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
        if ((!strcmp(c[2], "ui") || !strcmp(c[2], "menu")) && c[5][0] == '$' && c[5][1] == 'E') g_setup_e.push_back((uint32_t)strtoul(c[0], 0, 16));
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
    HS->opt_mask = 0;                                  // setup: no option answered
    HS->cf_ok = 1;
    for (int i = 0; i < 6; i++) HS->ext[i] = (float)(i & 1 ? 1 : -1);
    strcpy(HS->texname, "tex");
    UI_G32(S_SCREEN_W) = 640;
    UI_G32(S_SCREEN_H) = 480;
    for (int i = 0; i < 10; i++) { sprintf(g_friendly[i], "Friendly %d", i - 1); sprintf(g_text[i], "Text of %d", i - 1); }
    for (int k = 0; k < 8; k++)
        for (int v = 0; v < 8; v++) sprintf(g_enum[k][v], "E%d.%d", k, v);
    for (int t = 0; t < 4; t++)
        for (int r = 0; r < 16; r++) {
            if (r % 5 == 0) sprintf(g_tabs_str[t][r], "Car %d-%d", t, r);
            else if (r % 5 == 1) sprintf(g_tabs_str[t][r], "%d", 100 + 37 * r + t);
            else sprintf(g_tabs_str[t][r], "%d.%d", r * 3 + t, r);
        }
    for (uint32_t a : g_setup_e) call_orig(a, 0, {});
    call_orig(0x004779a0, 0, {});                      // UIBegin
    W.screen = (gxCanvas*)wv(sizeof(gxCanvas));
    W.screen->format = 4; W.screen->pixels = (uint8_t*)0x7b000000; W.screen->w = 640; W.screen->h = 480; W.screen->pitch = 1280;
    W.screen->cx1 = 640; W.screen->cy1 = 480;
    W.canvas2 = (gxCanvas*)wv(sizeof(gxCanvas));
    memcpy((void*)W.canvas2, (const void*)W.screen, sizeof(gxCanvas));
    W.canvas2->w = 320; W.canvas2->h = 200; W.canvas2->cx0 = 10; W.canvas2->cy0 = 5; W.canvas2->cx1 = 300; W.canvas2->cy1 = 190;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    // the root's car list and a locale
    W.carlist = (char*)wv(32 * 6);
    for (int i = 0; i < 6; i++) strcpy(W.carlist + 32 * i, k_cars[i]);
    UI_GP(char, 0x00504340) = W.carlist;
    UI_G32(0x00504344) = 6;
    W.locale = (uint8_t*)wv(0x40);
    UI_GP(uint8_t, S_LOCALE) = W.locale;
    *(const char**)(W.locale + 0x18) = wstr("mph");
    *(float*)(W.locale + 0x1c) = 1.0f;
    *(const char**)(W.locale + 0x28) = wstr("hp");
    *(float*)(W.locale + 0x2c) = 1.0f;
    W.opts = (GameOptions*)wv(sizeof(GameOptions));
    W.track_name = (char*)wv(0x80);
    W.car_name = (char*)wv(0x80);
    W.summary = (char*)wv(0x100);
    W.flags = (uint8_t*)wv(16);
    W.ints = (int32_t*)wv(64);
    for (int i = 0; i < 8; i++) W.texts[i] = (char*)wv(0x40);
    W.scratch = (uint8_t*)wv(0x400);
    g_recs = (MvRaceRecord*)wv(8 * sizeof(MvRaceRecord));
    // the objects, by their own constructors
    W.tc = (TrackChooser*)wv(sizeof(TrackChooser));
    call_orig(0x0049b2d0, 1, {U(W.tc), 0, 1, U(&W.flags[0]), U(W.track_name), U(W.opts)});
    W.tv = (TrackViewer*)wv(sizeof(TrackViewer));
    call_orig(0x0049ad60, 1, {U(W.tv), 0});
    W.cc = (CarChooser*)wv(sizeof(CarChooser));
    call_orig(0x0049d250, 1, {U(W.cc), 0, 0, U(W.car_name), 0, 0, 0x00406920, 0x00406910});
    W.cv = (CarViewer3D*)wv(sizeof(CarViewer3D));
    call_orig(0x0049bf50, 1, {U(W.cv), 0, 1, 0x00406920, 0x00406910});
    W.ov = (OpponentViewer*)wv(sizeof(OpponentViewer));
    call_orig(0x0049e2e0, 1, {U(W.ov), 0, U(W.opts), U(&W.ints[0])});
    call_orig(0x0049eaa0, 1, {U(W.ov), 0, U(wstr("viper"))});
    W.rov = (RaceOptionViewer*)wv(sizeof(RaceOptionViewer));
    call_orig(0x0049f120, 1, {U(W.rov), 0, U(&W.ints[1]), U(&W.ints[2]), U(W.opts), U(&W.flags[1]), U(W.summary)});
    W.rrt = (RaceResultTable*)wv(sizeof(RaceResultTable));
    call_orig(0x0048fe20, 1, {U(W.rrt), 0});
    W.hs = (HighScoreBoardTable*)wv(sizeof(HighScoreBoardTable));
    call_orig(0x0048fe20, 1, {U(W.hs), 0});
    W.hs->vtbl = (const void*)(uintptr_t)VT_HighScoreBoardTable;
    W.hs->mgr = g_mgr;
    W.bct = (BoardCustomText*)wv(sizeof(BoardCustomText));
    call_orig(0x0048fd50, 1, {U(W.bct), 0, U(wstr("Board text"))});
    call_orig(0x00490690, 0, {U(wstr("bemidji")), 1, 2, 0});
    W.bc = (BoardControl*)(uintptr_t)g_setup_ret;
    if (getenv("VP_DEBUG_WORLD")) {
        const void* ctl[] = {W.tc, W.tv, W.cc, W.cv, W.ov, W.rov, W.rrt, W.hs, W.bct, W.bc};
        for (const void* c : ctl) printf("  object %p: vtable %08x\n", c, c ? *(const uint32_t*)c : 0);
    }
    // a full-screen window with every control in a CustomWidget
    UIDialogItem* items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 16);
    UIDialogItem* it = items;
    it = it_(it, 23, 0, 10, 10, 0xb3, 0x8d, "", 0, W.tc, 0);
    it = it_(it, 23, 0, 200, 10, 0xb3, 0x8d, "", 0, W.tv, 0);
    it = it_(it, 23, 0, 10, 200, 0xb3, 0x8d, "", 0, W.cc, 0);
    it = it_(it, 23, 0, 200, 200, 0xb3, 0x8d, "", 0, W.cv, 0);
    it = it_(it, 23, 0, 400, 10, 0xb3, 0x8d, "", 0, W.ov, 0);
    it = it_(it, 23, 0, 400, 200, 0x100, 0x80, "", 0, W.rov, 0);
    it = it_(it, 23, 0, 20, 310, 0x230, 0x136, "", 0, W.hs, 0);
    it = it_(it, 23, 0, 250, 30, 0x168, 0x10, "", 0, W.bct, 0);
    it = it_(it, 23, 0, 30, 320, 0x230, 0x136, "", 0, W.bc, 0);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    call_orig(F_WidgetCreateWindow, 0, {0, 0, 0, 640, 480, 2});
    W.win = (WidgetWindow*)(uintptr_t)g_setup_ret;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    if (getenv("VP_DEBUG_WORLD"))
        for (UIDialogItem* q = items; q->type; q++) printf("  item type %d data %p (vtable %08x)\n", q->type, q->data, q->data ? *(uint32_t*)q->data : 0);
    call_orig(F_UIAddItems, 0, {U(W.win), U(items)});
    if (getenv("VP_DEBUG_WORLD"))
        for (int i = 0; i < W.win->count; i++) {
            const Widget* g = W.win->widgets[i];
            printf("  widget %p vtable %08x ctrl %p\n", (const void*)g, (uint32_t)(uintptr_t)g->vtbl, (void*)((const CustomWidget*)g)->ctrl);
        }
    // their own Create (the originals; Added ran as each CustomWidget was added)
    const void* ctl[] = {W.tc, W.tv, W.cc, W.cv, W.ov, W.rov, W.hs, W.bct, W.bc};
    for (const void* c : ctl) call_orig((*(const uint32_t* const*)c)[1], 1, {U(c), 0});
    call_orig(0x0049c060, 1, {U(&W.cc->viewer), 0});           // the chooser's viewer (its CustomWidget came with Added)
    for (int i = 0; i < W.win->count; i++) W.widgets.push_back((Widget*)W.win->widgets[i]);
    W.static_items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 4);
    it = W.static_items;
    it = it_(it, 5, 0, 20, 20, 0, 0, wstr("a static dialog's item"), 0, 0, 0xb);
    it = it_(it, 2, -1, 30, 100, 0, 0, wstr("OK"), 0, 0, 6);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
// the function-local Xlators: {guard, bit, Xlator, key}
static const uint32_t k_xl[][4] = {
    {0x0057a614, 1, 0x0057a710, 0x004f9194}, {0x0057a66c, 1, 0x0057a5e8, 0x004f9220}, {0x0057a66c, 2, 0x0057a680, 0x004f9244},
    {0x0057a66c, 4, 0x0057a700, 0x004f9250}, {0x0057a66c, 8, 0x0057a658, 0x004f9264}, {0x0057a67c, 1, 0x0057a6a0, 0x004f91c0},
    {0x0057a67c, 2, 0x0057a6d8, 0x004f91d8}, {0x0057a67c, 4, 0x0057a640, 0x004f91f4}, {0x0057a6c0, 0x01, 0x0057a670, 0x004f90f4},
    {0x0057a6c0, 0x02, 0x0057a690, 0x004f910c}, {0x0057a6c0, 0x04, 0x0057a628, 0x004f9120}, {0x0057a6c0, 0x08, 0x0057a6b0, 0x004f9130},
    {0x0057a6c0, 0x10, 0x0057a5f8, 0x004f9144}, {0x0057a6c0, 0x20, 0x0057a608, 0x004f915c}, {0x0057a6c0, 0x40, 0x0057a6c8, 0x004f9170},
    {0x0057a6c0, 0x80, 0x0057a618, 0x004f9180}, {0x0057b80c, 0x01, 0x0057b7e0, 0x004fa34c}, {0x0057b80c, 0x02, 0x0057b898, 0x004fa36c},
    {0x0057b80c, 0x04, 0x0057b830, 0x004fa384}, {0x0057b80c, 0x08, 0x0057b858, 0x004fa398}, {0x0057b80c, 0x10, 0x0057b868, 0x004fa3b0},
    {0x0057b80c, 0x20, 0x0057b7d0, 0x004fa3d0}, {0x0057b83c, 1, 0x0057b848, 0x004fa3f4}, {0x0057b840, 1, 0x0057b818, 0x004fa29c},
    {0x0057b840, 2, 0x0057b800, 0x004fa2b4}, {0x0057b840, 4, 0x0057b888, 0x004fa2cc}, {0x0057b8a8, 1, 0x0057b8c0, 0x004fa540},
    {0x0057b8a8, 2, 0x0057b998, 0x004fa560}, {0x0057b8a8, 4, 0x0057bc28, 0x004fa584}, {0x0057b8a8, 8, 0x0057bc48, 0x004fa5ac},
    {0x0057bc54, 0x01, 0x0057b928, 0x004fa6fc}, {0x0057bc54, 0x02, 0x0057bc98, 0x004fa714}, {0x0057bc54, 0x04, 0x0057b8b0, 0x004fa730},
    {0x0057bc54, 0x08, 0x0057b908, 0x004fa748}, {0x0057bc5c, 1, 0x0057b988, 0x004fa774}, {0x0057bc78, 0x01, 0x0057bc38, 0x004fa640},
    {0x0057bc78, 0x02, 0x0057b918, 0x004fa658}, {0x0057bc78, 0x04, 0x0057bc18, 0x004fa670}, {0x0057bc78, 0x08, 0x0057bc88, 0x004fa688},
    {0x0057bc78, 0x10, 0x0057b938, 0x004fa69c}, {0x0057bc78, 0x20, 0x0057b978, 0x004fa6b4}, {0x0057bc78, 0x40, 0x0057b8d0, 0x004fa6cc},
    {0x0057bc78, 0x80, 0x0057bc60, 0x004fa6e4}, {0x0057bd0c, 1, 0x0057bcf0, 0x004fa878}, {0x0057bd0c, 2, 0x0057bd20, 0x004fa890},
    {0x0057bd0c, 4, 0x0057bd10, 0x004fa8a8}, {0x0057bd74, 0x01, 0x0057bd98, 0x004fa9ec}, {0x0057bd74, 0x02, 0x0057be48, 0x004faa10},
    {0x0057bd74, 0x04, 0x0057bd68, 0x004faa38}, {0x0057bd74, 0x08, 0x0057bdd0, 0x004faa4c}, {0x0057be54, 1, 0x0057be88, 0x004faa78},
    {0x0057be78, 0x01, 0x0057bd88, 0x004fa924}, {0x0057be78, 0x02, 0x0057be60, 0x004fa938}, {0x0057be78, 0x04, 0x0057bdb0, 0x004fa950},
    {0x0057be78, 0x08, 0x0057bdc0, 0x004fa96c}, {0x0057be78, 0x10, 0x0057bdf0, 0x004fa984}, {0x0057be78, 0x20, 0x0057bd58, 0x004fa998},
    {0x0057be78, 0x40, 0x0057bd48, 0x004fa9b0}, {0x0057be78, 0x80, 0x0057bde0, 0x004fa9cc},
};
static const char k_chars[] = "abcdefghij klmnopqrstuvwxyz ABCDEFGH 0123456789 .,!-";
static void random_text(char* s, int maxlen) {
    const int n = irange(0, maxlen);
    for (int i = 0; i < n; i++) s[i] = k_chars[rnd() % (sizeof k_chars - 1)];
    s[n] = 0;
}
static void random_name(char* s, int maxlen) {                 // a car-ish name
    if (chance(60)) { strcpy(s, k_cars[rnd() % 6]); return; }
    random_text(s, maxlen);
}
static void randomize_world() {
    // the objects
    GameOptions* o = W.opts;
    o->realism = irange(0, 2); o->game_time = irange(0, 2); o->weather = irange(0, 2); o->field = chance(90) ? irange(0, 2) : irange(-1, 4);
    o->race_type = irange(0, 3); o->laps = irange(0, 30); o->event = chance(40) ? 0 : irange(0, 5); o->ghost = irange(0, 3);
    o->ai_strength = irange(0, 2); o->b24 = (uint8_t)(rnd() & 1); o->mirror = (uint8_t)(chance(90) ? rnd() & 1 : rnd());
    for (TrackViewer* v : {W.tv, &W.tc->viewer}) v->track = chance(85) ? irange(-1, 7) : irange(8, 9);
    W.tc->flags = (uint8_t)(rnd() & 7);
    for (CarViewer3D* v : {W.cv, &W.cc->viewer}) {
        v->car = chance(85) ? irange(-1, 5) : irange(6, 8);
        v->paint = chance(85) ? irange(0, 7) : irange(-3, 12);
        v->t = chance(85) ? range(-0.6f, 1.2f) : wildf();
        v->pose = chance(90) ? irange(0, 4) : irange(-1, 6);
        v->model = chance(80) ? 0x300 + irange(0, 3) : 0;
        v->wheel[0] = 0x400 + irange(0, 9); v->wheel[1] = 0x400 + irange(0, 9);
        v->tab = chance(80) ? (void*)g_tabs[rnd() & 3] : 0;
        if (chance(50)) sprintf(v->set, "%s.car", k_cars[rnd() % 6]);
        else if (chance(50)) v->set[0] = 0;
        for (int i = 0; i < 5; i++) { v->stat[i] = fval(0, 300); v->stat2[i] = fval(0, 9000); }
        v->istat = irange(-5, 700);
        random_text(v->text, 20);
        for (int i = 0; i < 9; i++) v->body.m[i] = fval(-1, 1);
        for (int i = 0; i < 3; i++) v->body.p[i] = fval(-5, 5);
    }
    W.cc->b1f8 = (uint8_t)(rnd() & 1); W.cc->b1f9 = (uint8_t)(rnd() & 1);
    OpponentViewer* ov = W.ov;
    ov->type = chance(90) ? irange(0, 2) : irange(-1, 3);
    ov->saved[0] = irange(-1, 3); ov->saved[1] = irange(-1, 3);
    ov->created = (uint8_t)(chance(50));
    ov->ghost = (uint8_t)(rnd() & 1);
    ov->model = chance(70) ? 0x200 + irange(0, 5) : 0;
    random_name(ov->car, 24);
    if (chance(50)) sprintf(ov->set, "%s.car", k_cars[rnd() % 6]);
    else ov->set[0] = 0;
    for (int i = 0; i < 3; i++) ov->body.p[i] = fval(-5, 5);
    static const char* const bct_texts[] = {"Board text", "50%% off", "100%", "%%%%", "", "Arcade: Simulation"};
    W.bct->text = bct_texts[rnd() % 6];                  // (a '%' the game's printf reads no argument for)
    RaceOptionViewer* rv = W.rov;
    rv->event = irange(0, 5);
    rv->ai_count = fval(0, 9);
    W.flags[0] = (uint8_t)(rnd() & 1); W.flags[1] = (uint8_t)(chance(90) ? rnd() & 1 : rnd());
    for (int i = 0; i < 4; i++) W.ints[i] = irange(-2, 12);
    random_text(W.track_name, 30); random_text(W.car_name, 30); random_text(W.summary, 60);
    for (int i = 0; i < 8; i++) random_text(W.texts[i], i < 4 ? 20 : 40);
    for (Widget* g : W.widgets) { g->visible = chance(85); g->enabled = chance(85); g->dirty = chance(50); g->over = chance(30); g->focus = chance(30); }
    for (int i = 0; i < 8; i++) {
        MvRaceRecord* r = &g_recs[i];
        r->year = (int16_t)irange(1990, 2030); r->day = (uint8_t)irange(1, 31); r->month = (uint8_t)irange(1, 12);
        r->lap_time = chance(20) ? -1.0f : fval(-5, 200); r->top_speed = chance(20) ? -1.0f : fval(-5, 250);
        r->race_time = chance(20) ? -1.0f : fval(-5, 900);
        random_text(r->car_name, 12);
        random_text(r->driver_name, 20);
        r->flags = (uint8_t)rnd();
    }
    // the stubs' world
    HS->time = (int32_t)rnd();
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->grab_fail_at = chance(20) ? irange(1, 6) : -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->script_n = HS->script_pos = 0;
    HS->opt_mask = rnd();
    for (int i = 0; i < 8; i++) HS->opt_val[i] = chance(85) ? irange(-1, 7) : irange(-3, 12);
    HS->lap_seed = (int32_t)rnd();
    HS->model_next = 0; HS->model_zero = chance(10);
    HS->cf_ok = chance(90);
    HS->rec_mask = (int32_t)rnd();
    HS->tab_seed = irange(0, 15);
    HS->tab_null = chance(10);
    g_xl_seed = chance(80) ? 0 : rnd();
    for (int i = 0; i < 6; i++) HS->ext[i] = chance(10) ? (i ? HS->ext[i - 1] : 0.0f) : fval(-3, 3);
    for (int i = 0; i < 10; i++) HS->cf[i] = fval(-100, 100);
    sprintf(HS->texname, "t%d", irange(0, 99));
}
// .data after poisoning, and each round: the statics the code follows and the function-local state
static void randomize_statics() {
    // the ui library's (world_ui.cpp's)
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_GU32(S_DIALOG_IDLE) = 0;
    UI_G8(S_EXIT) = 0;
    UI_GF(S_UI_DT) = chance(10) ? wildf() : range(0.0f, 0.25f);
    UI_GP(void, S_CURSOR) = 0;
    UI_G32(S_MOUSE_X) = irange(0, 639); UI_G32(S_MOUSE_Y) = irange(0, 479);
    // this group's
    UI_GP(char, 0x00504340) = W.carlist;
    UI_G32(0x00504344) = 6;
    UI_GP(uint8_t, S_LOCALE) = W.locale;
    *(float*)(W.locale + 0x1c) = chance(50) ? 1.0f : fval(0, 3);
    *(float*)(W.locale + 0x2c) = chance(50) ? 1.0f : fval(0, 3);
    W.locale[0x38] = (uint8_t)(chance(50) ? 0 : rnd());
    UI_GP(const void, S_BOARD_MGR) = g_mgr;
    UI_GP(void, S_BOARD_TABLE) = W.hs;
    UI_GP(void, S_TRACK_CHOOSER) = W.tc;
    UI_GP(void, S_CAR_CHOOSER) = W.cc;
    UI_GP(void, S_OPP_VIEWER) = W.ov;
    UI_G32(S_MR_DEVICE) = irange(0, 3);
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
    // the details dialogs' static UIDialogs: unset, or set by an earlier call (to a list of the harness's)
    struct { uint32_t guard; uint8_t bit; uint32_t dlg; } sd[] = {{0x0057b80c, 0x40, S_TRK_DIALOG}, {0x0057bc54, 0x10, S_CAR_DIALOG}};
    for (auto& d : sd) {
        if (chance(60)) UI_G8(d.guard) = (uint8_t)(UI_G8(d.guard) & ~d.bit);
        else {
            UI_G8(d.guard) = (uint8_t)(UI_G8(d.guard) | d.bit);
            UI_GP(char, d.dlg) = W.texts[0];
            UI_GU32(d.dlg + 4) = chance(50) ? 0x004fa3e8u : 0u;
            UI_GU32(d.dlg + 8) = 0xfffffffeu;
            UI_GU32(d.dlg + 0xc) = U(W.static_items);
            UI_GU32(d.dlg + 0x10) = 0;
        }
    }
    if (chance(50)) UI_G8(0x0057bd0c) = (uint8_t)(UI_G8(0x0057bd0c) & ~8);     // OpponentViewer's labels
    else {
        UI_G8(0x0057bd0c) = (uint8_t)(UI_G8(0x0057bd0c) | 8);
        for (int i = 0; i < 3; i++) UI_GP(char, S_OPP_LABELS + 4 * i) = W.texts[1 + i];
    }
    static const uint32_t cf_at[10] = {0x0057b9e0, 0x0057b9e4, 0x0057b9f8, 0x0057bb68, 0x0057bb6c, 0x0057bb70, 0x0057bb74, 0x0057bb78, 0x0057bb7c, 0x0057b9ec};
    for (int i = 0; i < 10; i++) UI_GF(cf_at[i]) = fval(-100, 100);
}
// a random input script (for the functions that run a dialog)
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x123, 0x124, 0x125, 0x127, 'q', 'a', '5'};
    const int n = irange(0, maxops);
    for (int i = 0; i < n && i < 62; i++) {
        int32_t* op = HS->script[i];
        const uint32_t r = rnd() % 10;
        op[1] = op[2] = 0;
        if (r < 4) { op[0] = OP_MOVE_W; op[1] = irange(0, 40); op[2] = irange(0, 479); }
        else if (r < 5) { op[0] = OP_MOVE_XY; op[1] = irange(-10, 650); op[2] = irange(-10, 490); }
        else if (r < 6) op[0] = OP_DOWN;
        else if (r < 7) op[0] = OP_UP;
        else if (r < 9) { op[0] = OP_KEY; op[1] = keys[rnd() % (sizeof keys / sizeof keys[0])]; }
        else op[0] = OP_NOP;
        if (op[0] == OP_MOVE_W && chance(70)) {
            HS->script[i + 1][0] = OP_DOWN; HS->script[i + 2][0] = OP_UP;
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
        {0x004f665c, 0x004f6698},             // the ui library's Xlators' keys
        {0x004f6b78, 0x004f7228}, {0x004f7230, 0x004f7300},   // ui / _widget / widget / uistyle strings
        {0x004efbf8, 0x004efbfc},             // the current canvas
        {0x004f9000, 0x004f9300},             // board.obj's strings and statics
        {0x004fa1f0, 0x004faac0},             // trkview / carview / oppview / optview strings, tables and statics
        {0x005024b8, 0x005024bc},             // __adjust_fdiv
        {0x005228d4, 0x005228d8}, {0x005228f4, 0x005228f8},   // the screen's size
        {0x00578cd0, 0x00578cdc}, {0x00578d50, 0x00578d5c}, {0x00578d60, 0x00578d6c}, {0x00578e88, 0x00578e94},
        {0x00578e98, 0x00578ea4}, {0x00578eb0, 0x00578ebc}, {0x00578ec0, 0x00578ecc}, {0x00578ef8, 0x00578f04},
        {0x00579050, 0x0057905c},             // the ui library's Xlators
        {0x00578d08, 0x00578d40},             // the end item
        {0x00578d48, 0x00578d4c}, {0x00578e78, 0x00578e7c}, {0x00578f40, 0x00578f44}, {0x00578e80, 0x00578e84},
        {0x00579730, 0x00579778},             // the running window, the window table
    };
    for (auto& k : keep) memcpy(DATA + (k[0] - 0x004e1000), keepfrom.data + (k[0] - 0x004e1000), k[1] - k[0]);
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
// a byte argument as a caller pushes it: the register's other bytes are whatever it held
static uint32_t b8(uint32_t v) { return chance(50) ? (rnd() & ~0xffu) | (v & 0xff) : v; }
static uint32_t canvas_arg() { return chance(70) ? U(W.screen) : U(W.canvas2); }
static uint32_t name_arg() {
    static const char* n[] = {"viper1.mod", "cobra1.mod", "gts3.mod", "rt10.x.mod", "zr1.mod"};
    return chance(90) ? U(n[rnd() % 5]) : U(W.texts[rnd() % 4]);
}
static uint32_t track_arg() {
    return chance(85) ? U(k_tracks[rnd() % 9]) : U(W.texts[rnd() % 4]);
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
    memset(W.scratch, 0xa5, 0x400);
    const bool ctor = cls[0] && !strcmp(meth, cls);
#define IS(s) (!strcmp(nm, s))
#define M(s) (!strcmp(meth, s))
    if (strstr(meth, "deleting destructor")) a[0] = rnd() & 3;
    if (!strcmp(cls, "TrackViewer")) {
        w[0] = chance(50) ? U(W.tv) : U(&W.tc->viewer);
        if (ctor) w[0] = U(W.scratch);
        else if (M("Draw")) a[0] = canvas_arg();
        else if (M("SetTrack")) a[0] = (uint32_t)(chance(90) ? irange(-1, 8) : irange(-3, 12));
        else if (M("set_stamp")) a[0] = chance(80) ? U("bemidji.stp") : U(W.texts[rnd() % 4]);
    } else if (!strcmp(cls, "TrackChooser")) {
        w[0] = U(W.tc);
        if (ctor) { w[0] = U(W.scratch); a[0] = b8(rnd() & 7); a[1] = U(&W.flags[2]); a[2] = U(W.track_name); a[3] = U(W.opts); }
        else if (M("Draw")) a[0] = canvas_arg();
        else if (M("Callback")) { a[0] = rnd() % 3; a[1] = U(&W.opts->race_type); }
    } else if (!strcmp(cls, "CarViewer3D")) {
        w[0] = chance(50) ? U(W.cv) : U(&W.cc->viewer);
        if (ctor) { w[0] = U(W.scratch); a[0] = (uint32_t)irange(-1, 7); a[1] = 0x00406920; a[2] = 0x00406910; }
        else if (M("Draw")) a[0] = canvas_arg();
        else if (M("SetModel")) a[0] = chance(15) ? 0 : name_arg();
        else if (M("SetCar")) a[0] = (uint32_t)irange(-2, 8);
        else if (M("SetPaintJob")) a[0] = (uint32_t)irange(-3, 10);
    } else if (!strcmp(cls, "CarChooser")) {
        w[0] = U(W.cc);
        if (ctor) { w[0] = U(W.scratch); a[0] = b8(rnd() & 1); a[1] = U(W.car_name); a[2] = (uint32_t)irange(-1, 7); a[3] = b8(rnd() & 1); a[4] = 0x00406920; a[5] = 0x00406910; }
        else if (M("Draw")) a[0] = canvas_arg();
    } else if (!strcmp(cls, "OpponentViewer")) {
        w[0] = U(W.ov);
        if (ctor) { w[0] = U(W.scratch); a[0] = U(W.opts); a[1] = U(&W.ints[3]); }
        else if (M("Draw")) a[0] = canvas_arg();
        else if (M("Callback")) { a[0] = (uint32_t)irange(0, 2); a[1] = U(&W.ov->type); }
        else if (M("SetCar")) { char* s = W.texts[4]; random_name(s, 24); a[0] = U(s); }
        else if (M("SetModel")) a[0] = chance(15) ? 0 : chance(10) ? U("") : name_arg();
        else if (M("next_cb") || M("prev_cb")) a[0] = rnd();
    } else if (!strcmp(cls, "RaceOptionViewer")) {
        w[0] = U(W.rov);
        if (ctor) { w[0] = U(W.scratch); a[0] = U(&W.ints[1]); a[1] = U(&W.ints[2]); a[2] = U(W.opts); a[3] = U(&W.flags[1]); a[4] = U(W.summary); }
        else if (M("Draw")) a[0] = canvas_arg();
        else if (M("Callback")) { a[0] = rnd() & 1; a[1] = U(&W.opts->field); }
    } else if (!strcmp(cls, "RaceResultTable")) {
        w[0] = chance(50) ? U(W.rrt) : U(W.hs);
        if (ctor) w[0] = U(W.scratch);
        else if (M("draw_legend")) { a[0] = (uint32_t)irange(-20, 400); a[1] = b8(rnd() & 1); a[2] = b8(rnd() & 1); }
        else if (M("draw_element")) {
            a[0] = canvas_arg(); a[1] = (uint32_t)irange(-20, 400); a[2] = U(&g_recs[rnd() & 7]); a[3] = b8(rnd() & 1); a[4] = b8(rnd() & 1); a[5] = b8(rnd() & 1);
        }
    } else if (!strcmp(cls, "HighScoreBoardTable")) {
        w[0] = U(W.hs);
        if (M("Draw")) a[0] = canvas_arg();
    } else if (!strcmp(cls, "BoardCustomText")) {
        w[0] = U(W.bct);
        if (ctor) { w[0] = U(W.scratch); a[0] = U(W.texts[rnd() % 8]); }
        else if (M("Draw")) a[0] = canvas_arg();
    } else if (!strcmp(cls, "BoardControl")) {
        w[0] = U(W.bc);
        if (M("Draw")) a[0] = canvas_arg();
    } else if (IS("get_track_order")) { a[0] = (uint32_t)irange(-9, 9); a[1] = chance(85) ? U(k_tracks[rnd() % 8]) : track_arg(); }
    else if (IS("is_test_track")) a[0] = chance(30) ? U("HeLL") : track_arg();
    else if (IS("Hermite")) a[0] = chance(20) ? fbits(wildf()) : fbits(range(-1.5f, 1.5f));
    else if (IS("BoardCreateControl") || IS("BoardDo")) { a[0] = track_arg(); a[1] = rnd() % 3; a[2] = rnd() % 4; a[3] = b8(rnd() & 1); }
    else a[0] = rnd();                                   // the button callbacks (an id)
#undef IS
#undef M
    return true;
}
static bool is_modal(const char* nm) {
    return strstr(nm, "details") || strstr(nm, "BoardDo") || strstr(nm, "do_board") || strstr(nm, "board_cb") ||
           strstr(nm, "car_details_cb") || strstr(nm, "::paint");
}

#if MENU_FIXES
// ---- the fixes, each on its bad case (directed_fix_tests) -----------------------------------------------------------------
// The rewrite alone (the original would crash or overrun there -- this program's stack among what it would take), from the
// pristine world with the case set up: it must return cleanly (no fault, the bytes popped, ebx / esi / edi / ebp kept),
// write nothing outside what it's given (every other byte of .data/.bss/.idata and the arena compared with before) and
// give what the fix promises. Each fix's boundary case (the longest input that fits) runs on the original and the rewrite
// from the same state and must give the same memory, call logs and result.
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
static bool fx_clean(const Ent& f, const Result& r) {
    return !r.fault && r.pops == (f.fast ? 4u * (uint32_t)f.nstack : 0u) && r.regs[0] == 0x0b0b0b0b && r.regs[1] == 0x05050505 &&
           r.regs[2] == 0x0d0d0d0d && r.regs[3] == 0x0e0e0e0e;
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
// a function-local Xlator built, with its text (fresh: read as it is; stale: refreshed through the stub)
static void fx_xl(uint32_t xl, const char* text, bool fresh) {
    for (auto& x : k_xl)
        if (x[2] == xl) {
            UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) | x[1]);
            UI_GU32(x[2]) = x[3];
            UI_GU32(x[2] + 4) = U(text ? text : xl_text(x[3]));
            UI_GU32(x[2] + 8) = fresh ? UI_GU32(S_XLATOR_COOKIE) : ~UI_GU32(S_XLATOR_COOKIE);
        }
}
static const char* const k_labels[3] = {"The pack", "Clock", "Ghost car"};
// the tests' world: pristine, the stubs' hooks off, every function-local Xlator built (fresh), the statics the code follows
static void fx_reset() {
    mem_load(g_pristine);
    g_fx_track = g_fx_tab6 = g_fx_xl = 0;
    for (auto& e : g_fx_enum) e = 0;
    g_fx_real_tex = false;
    g_rec_mark = 0;
    g_xl_seed = 0;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_GU32(S_DIALOG_IDLE) = 0;
    UI_G8(S_EXIT) = 0;
    UI_GP(void, S_CURSOR) = 0;
    UI_GP(uint8_t, S_LOCALE) = W.locale;
    *(float*)(W.locale + 0x1c) = 1.0f;
    *(float*)(W.locale + 0x2c) = 1.0f;
    W.locale[0x38] = 0;
    UI_GP(const void, S_BOARD_MGR) = g_mgr;
    UI_GP(void, S_BOARD_TABLE) = W.hs;
    UI_GP(void, S_TRACK_CHOOSER) = W.tc;
    UI_GP(void, S_CAR_CHOOSER) = W.cc;
    UI_GP(void, S_OPP_VIEWER) = W.ov;
    UI_G32(S_MR_DEVICE) = 2;
    for (auto& x : k_xl) fx_xl(x[2], 0, true);
    UI_G8(0x0057bd0c) = (uint8_t)(UI_G8(0x0057bd0c) | 8);
    for (int i = 0; i < 3; i++) UI_GP(const char, S_OPP_LABELS + 4 * i) = k_labels[i];
    UI_G8(0x0057b80c) = (uint8_t)(UI_G8(0x0057b80c) & ~0x40);
    UI_G8(0x0057bc54) = (uint8_t)(UI_G8(0x0057bc54) & ~0x10);
    HS->frame_limit = 40; HS->frames = 0; HS->script_n = HS->script_pos = 0;
    HS->cf_ok = 1; HS->tab_null = 0; HS->model_zero = 0; HS->model_next = 0;
}
// a string of n c's and then tail (in one of eight buffers of this program's)
static char g_fx_s[8][0x400];
static const char* mk(int k, char c, int n, const char* tail = "") {
    memset(g_fx_s[k], c, (size_t)n);
    strcpy(g_fx_s[k] + n, tail);
    return g_fx_s[k];
}
static Span widget_of(const void* ctl) { return {(const void*)((const MvCtl*)ctl)->widget, sizeof(CustomWidget)}; }
// callees hooked to their rewrites, as in the DLL (a rewrite calls this group's functions by their v1.0 address, which
// here is the original), for the test's lifetime
struct FxHook {
    struct Saved { uint32_t at; uint8_t b[5]; };
    std::vector<Saved> saved;
    FxHook(std::initializer_list<const char*> names) {
        for (const char* n : names) {
            const Ent& e = fx_fn(n);
            Saved s;
            s.at = e.v10;
            memcpy(s.b, (const void*)(uintptr_t)e.v10, 5);
            saved.push_back(s);
            patch_jmp(e.v10, e.fn);
        }
    }
    ~FxHook() { for (const Saved& s : saved) memcpy((void*)(uintptr_t)s.at, s.b, 5); }
};
// the remap entry (0x28 bytes: two 16-byte names, a value and a pointer) as the fixed SetModel leaves it: "<base>.tex"
// from its start, the texture name's first 23 characters and a terminator from +0x10, +0x20 and +0x24 zeroed
static void remap_expect(uint8_t* e, const uint8_t* before, const char* base, const char* tex) {
    char t[0x80];
    memcpy(e, before, 0x28);
    sprintf(t, "%s.tex", base);
    memcpy(e, t, strlen(t) + 1);
    const size_t n = strlen(tex) < 0x17 ? strlen(tex) : 0x17;
    memcpy(e + 0x10, tex, n);
    e[0x10 + n] = 0;
    memset(e + 0x20, 0, 8);
}
// the game's own printf (vsprintf, 0x4cf7c0) on fmt with its arguments on a no-access page: whether it reads one
static uint8_t* g_noaccess;
static bool game_reads_args(const char* fmt) {
    static char out[0x2000];
    typedef int(__cdecl * VSprintf_t)(char*, const char*, void*);
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] { ((VSprintf_t)(uintptr_t)0x004cf7c0)(out, fmt, g_noaccess); },
               [](EXCEPTION_POINTERS* e) {
                   return e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                                          : EXCEPTION_CONTINUE_SEARCH;
               })) {
        return true;
    }
#else
    __try {
        ((VSprintf_t)(uintptr_t)0x004cf7c0)(out, fmt, g_noaccess);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
#endif
    return false;
}

static int directed_fix_tests() {
    Result r;
    uint32_t o;
    char m[256];
    // ---- 6. TrackViewer::SetTrack: a long track name ----
    {
        const Ent& f = fx_fn("TrackViewer::SetTrack");
        fx_reset();
        g_fx_track = mk(0, 't', 200);
        mem_save(g_snap);
        r = fx_run(f, true, {U(W.tv), 0, 5});
        fx_check(fx_clean(f, r), "TrackViewer::SetTrack, a 200-character name: a clean return", &r);
        fx_check(!strcmp(g_rec_stamp, mk(1, 't', 74, ".stp")) && W.tv->track == 5,
                 "TrackViewer::SetTrack, a 200-character name: the map asked for is its first 74 characters and .stp");
        o = fx_outside(g_snap, {{W.tv, sizeof(TrackViewer)}, widget_of(W.tv)});
        fx_check(o == 0, "TrackViewer::SetTrack, a 200-character name: nothing written but the viewer", 0, o);
        fx_reset();
        g_fx_track = mk(0, 't', 74);
        fx_same(f, {U(W.tv), 0, 5}, "TrackViewer::SetTrack, a 74-character name (the longest that fits)");
    }
    // ---- 7. CarViewer3D::SetModel: no '.', a '.' first, long names, a long texture name ----
    {
        const Ent& f = fx_fn("CarViewer3D::SetModel");
        CarViewer3D* v = W.cv;
        const struct { const char* name; const char* base; const char* cf; bool orig_faults; } cases[] = {
            {"viperxyz", "viperxyz", "viperxyz.cf", true},
            {".mod", ".mod", ".mod.cf", false},
            {mk(0, 'a', 55, "0.mod"), mk(1, 'a', 31), mk(2, 'a', 55, ".cf"), false},
            {mk(3, 'b', 395, "0.mod"), mk(4, 'b', 31), mk(5, 'b', 251, ".cf"), false},
        };
        for (const auto& c : cases) {
            fx_reset();
            g_fx_real_tex = true;
            v->model = 0;
            v->paint = 0;
            char tex[0x80];
            sprintf(tex, "~%s.tex", c.base);
            uint8_t before[0x28], want[0x28];
            memcpy(before, (const void*)(uintptr_t)S_REMAP_CAR, 0x28);
            remap_expect(want, before, c.base, tex);
            const size_t len = strlen(c.name);
            mem_save(g_snap);
            if (c.orig_faults) {
                r = fx_run(f, false, {U(v), 0, U(c.name)});
                sprintf(m, "CarViewer3D::SetModel, \"%s\": the original faults (the case is real)", c.name);
                fx_check(r.fault != 0, m);
                mem_load(g_snap);
            }
            g_rec_cf[0] = g_rec_remap_load[0] = 0;
            r = fx_run(f, true, {U(v), 0, U(c.name)});
            sprintf(m, "CarViewer3D::SetModel, a %u-character name (%.12s...): a clean return", (unsigned)len, c.name);
            fx_check(fx_clean(f, r), m, &r);
            sprintf(m, "CarViewer3D::SetModel, a %u-character name (%.12s...): the model, the remap entry and the .cf as promised",
                    (unsigned)len, c.name);
            const bool ok = !strcmp(g_rec_remap_load, c.name) && !memcmp(g_rec_remap, want, 0x28) &&
                            !memcmp((const void*)(uintptr_t)S_REMAP_CAR, want, 0x28) && !strcmp(g_rec_cf, c.cf) && v->model != 0;
            fx_check(ok, m);
            if (!ok) {
                printf("    loaded \"%.40s\", .cf \"%.40s\", model %x; the remap entry / wanted:\n     ", g_rec_remap_load, g_rec_cf, v->model);
                for (int i = 0; i < 0x28; i++) printf(" %02x", g_rec_remap[i]);
                printf("\n     ");
                for (int i = 0; i < 0x28; i++) printf(" %02x", want[i]);
                printf("\n");
            }
            o = fx_outside(g_snap, {{v, sizeof(CarViewer3D)}, widget_of(v), {(void*)(uintptr_t)S_REMAP_CAR, 0x28},
                                    {(void*)(uintptr_t)S_CARFILE, 0x1b8}});
            sprintf(m, "CarViewer3D::SetModel, a %u-character name (%.12s...): nothing written past the remap entry or the viewer",
                    (unsigned)len, c.name);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        g_fx_real_tex = true;
        fx_same(f, {U(v), 0, U(mk(0, 'c', 18, "0.mod"))}, "CarViewer3D::SetModel, an 18-character base (\"~<base>.tex\" fills the entry)");
    }
    // ---- 7. OpponentViewer::SetModel ----
    {
        const Ent& f = fx_fn("OpponentViewer::SetModel");
        OpponentViewer* ov = W.ov;
        const struct { const char* name; const char* set; bool loads; bool orig_faults; } cases[] = {
            {"viper", "viper.car", true, true},
            {mk(0, 'e', 35, "1.mod"), "", false, false},
            {mk(1, 'f', 28, "1.x"), "", false, false},
            {mk(2, 'g', 27, "1.x"), mk(3, 'g', 27, ".car"), true, false},
        };
        for (const auto& c : cases) {
            fx_reset();
            g_fx_real_tex = true;
            ov->model = 0;
            ov->set[0] = 0;
            char base[0x40], tex[0x80];
            strcpy(base, c.set);
            if (char* d = strrchr(base, '.')) *d = 0;
            if (!_stricmp(base, "viper")) strcpy(tex, "~paint0.tex");
            else sprintf(tex, "~%s.tex", base);
            uint8_t before[0x28], want[0x28], car[0x20];
            memcpy(before, (const void*)(uintptr_t)S_REMAP_OPP, 0x28);
            memcpy(car, ov->car, 0x20);
            remap_expect(want, before, base, tex);
            if (!c.loads) memcpy(want, before, 0x28);
            const size_t len = strlen(c.name);
            mem_save(g_snap);
            if (c.orig_faults) {
                r = fx_run(f, false, {U(ov), 0, U(c.name)});
                sprintf(m, "OpponentViewer::SetModel, \"%s\": the original faults (the case is real)", c.name);
                fx_check(r.fault != 0, m);
                mem_load(g_snap);
            }
            g_rec_mustload[0] = g_rec_load[0] = 0;
            r = fx_run(f, true, {U(ov), 0, U(c.name)});
            sprintf(m, "OpponentViewer::SetModel, a %u-character name (%.12s...): a clean return", (unsigned)len, c.name);
            fx_check(fx_clean(f, r), m, &r);
            if (c.loads) {
                sprintf(m, "OpponentViewer::SetModel, a %u-character name (%.12s...): its set \"%s\" loaded, its model, its remap entry",
                        (unsigned)len, c.name, c.set);
                fx_check(!strcmp(ov->set, c.set) && !strcmp(g_rec_mustload, c.set) && !strcmp(g_rec_load, c.name) && ov->model != 0 &&
                             !memcmp((const void*)(uintptr_t)S_REMAP_OPP, want, 0x28),
                         m);
            } else {
                sprintf(m, "OpponentViewer::SetModel, a %u-character name (%.12s...): too long for its set: no car", (unsigned)len, c.name);
                fx_check(ov->set[0] == 0 && ov->model == 0 && !fx_logged('RSML') && !fx_logged('MRLD'), m);
            }
            sprintf(m, "OpponentViewer::SetModel, a %u-character name (%.12s...): the car's name after the set untouched",
                    (unsigned)len, c.name);
            fx_check(!memcmp(car, ov->car, 0x20), m);
            o = fx_outside(g_snap, {{ov->set, 0x20}, {(void*)&ov->model, 4}, widget_of(ov), {(void*)(uintptr_t)S_REMAP_OPP, 0x28}});
            sprintf(m, "OpponentViewer::SetModel, a %u-character name (%.12s...): nothing written past the set or the remap entry",
                    (unsigned)len, c.name);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        g_fx_real_tex = true;
        ov->set[0] = 0;
        fx_same(f, {U(ov), 0, U(mk(0, 'h', 18, "1.mod"))}, "OpponentViewer::SetModel, an 18-character base (\"~<base>.tex\" fills the entry)");
    }
    // ---- 7. OpponentViewer::SetCar ----
    {
        const Ent& f = fx_fn("OpponentViewer::SetCar");
        OpponentViewer* ov = W.ov;
        fx_reset();
        ov->created = 0;
        mem_save(g_snap);
        r = fx_run(f, true, {U(ov), 0, U(mk(0, 'k', 100))});
        fx_check(fx_clean(f, r) && !strcmp(ov->car, mk(1, 'k', 31)), "OpponentViewer::SetCar, a 100-character name: its first 31", &r);
        o = fx_outside(g_snap, {{ov->car, 0x20}});
        fx_check(o == 0, "OpponentViewer::SetCar, a 100-character name: nothing written past the car's name", 0, o);
        {
            FxHook h({"OpponentViewer::UpdateOpponent", "OpponentViewer::SetModel"});
            fx_reset();                                     // created, a ghost car: UpdateOpponent, "<31 k>1.mod", no car
            ov->created = 1;
            ov->type = 2;
            ov->set[0] = 0;
            mem_save(g_snap);
            r = fx_run(f, true, {U(ov), 0, U(mk(0, 'k', 100))});
            fx_check(fx_clean(f, r) && ov->model == 0 && ov->set[0] == 0 && !fx_logged('RSML'),
                     "OpponentViewer::SetCar, a 100-character ghost car (the fixed callees): a clean return, no car", &r);
            o = fx_outside(g_snap, {{ov, sizeof(OpponentViewer)}, {W.opts, sizeof(GameOptions)}, widget_of(ov)});
            fx_check(o == 0, "OpponentViewer::SetCar, a 100-character ghost car: nothing written past the viewer", 0, o);
        }
        fx_reset();
        ov->created = 0;
        fx_same(f, {U(ov), 0, U(mk(0, 'k', 31))}, "OpponentViewer::SetCar, a 31-character name (the longest that fits)");
    }
    // ---- 7, 10. OpponentViewer::UpdateOpponent: an unterminated car name, a long label ----
    {
        const Ent& f = fx_fn("OpponentViewer::UpdateOpponent");
        OpponentViewer* ov = W.ov;
        FxHook* h = new FxHook({"OpponentViewer::SetModel"});
        fx_reset();
        memset(ov->car, 'c', 0x20);
        ov->type = 2;
        ov->set[0] = 0;
        mem_save(g_snap);
        r = fx_run(f, true, {U(ov), 0});
        fx_check(fx_clean(f, r) && ov->model == 0 && !strcmp(ov->label, k_labels[2]),
                 "OpponentViewer::UpdateOpponent, a car name filling its field: a clean return, no car", &r);
        o = fx_outside(g_snap, {{ov, sizeof(OpponentViewer)}, {W.opts, sizeof(GameOptions)}, widget_of(ov)});
        fx_check(o == 0, "OpponentViewer::UpdateOpponent, a car name filling its field: nothing written past the viewer", 0, o);
        fx_reset();
        UI_GP(const char, S_OPP_LABELS) = mk(0, 'l', 100);
        ov->type = 0;
        mem_save(g_snap);
        r = fx_run(f, true, {U(ov), 0});
        fx_check(fx_clean(f, r) && !strcmp(ov->label, mk(1, 'l', 34)), "OpponentViewer::UpdateOpponent, a 100-character label: its first 34", &r);
        o = fx_outside(g_snap, {{ov, offsetof(OpponentViewer, frame_stamp)}, {(void*)&ov->model, 4}, {W.opts, sizeof(GameOptions)}, widget_of(ov)});
        fx_check(o == 0, "OpponentViewer::UpdateOpponent, a 100-character label: nothing written past it (the stamps kept)", 0, o);
        delete h;
        fx_reset();
        UI_GP(const char, S_OPP_LABELS) = mk(0, 'l', 34);
        ov->type = 0;
        fx_same(f, {U(ov), 0}, "OpponentViewer::UpdateOpponent, a 34-character label (the longest that fits)");
        fx_reset();
        strcpy(ov->car, mk(0, 'c', 26));
        ov->type = 2;
        fx_same(f, {U(ov), 0}, "OpponentViewer::UpdateOpponent, a 26-character ghost car (\"<car>1.mod\" fills the original's buffer)");
    }
    // ---- 8. CarViewer3D::UpdateCar: a long .tab text ----
    {
        const Ent& f = fx_fn("CarViewer3D::UpdateCar");
        CarViewer3D* v = W.cv;
        fx_reset();
        v->car = 0;
        strcpy(v->set, "viper.car");
        v->tab = g_tabs[0];
        const CarCountFn cnt = v->count;
        const CarNameFn nm = v->name_of;
        g_fx_tab6 = mk(0, 'q', 100);
        mem_save(g_snap);
        r = fx_run(f, true, {U(v), 0});
        fx_check(fx_clean(f, r) && !strcmp(v->text, mk(1, 'q', 31)) && v->count == cnt && v->name_of == nm,
                 "CarViewer3D::UpdateCar, a 100-character text: its first 31, the car list's callbacks kept", &r);
        o = fx_outside(g_snap, {{v, sizeof(CarViewer3D)}, widget_of(v), {(void*)(uintptr_t)S_REMAP_CAR, 0x28}, {(void*)(uintptr_t)S_CARFILE, 0x1b8}});
        fx_check(o == 0, "CarViewer3D::UpdateCar, a 100-character text: nothing written past the viewer", 0, o);
        fx_reset();
        v->car = 0;
        strcpy(v->set, "viper.car");
        v->tab = g_tabs[0];
        g_fx_tab6 = mk(0, 'q', 31);
        fx_same(f, {U(v), 0}, "CarViewer3D::UpdateCar, a 31-character text (the longest that fits)");
    }
    // ---- 9. CarChooser::update_car: huge stats, long units ----
    {
        const Ent& f = fx_fn("CarChooser::update_car");
        CarChooser* cc = W.cc;
        CarViewer3D* v = &cc->viewer;
        fx_reset();
        const char* unit = mk(0, 'u', 300);
        const char* lunit = mk(1, 'v', 300);
        const char* punit = mk(2, 'w', 300);
        g_fx_xl = unit;
        for (uint32_t xl : {0x0057b8c0u, 0x0057b998u, 0x0057bc28u, 0x0057bc48u}) fx_xl(xl, 0, false);   // refreshed: the long unit
        *(const char**)(W.locale + 0x18) = lunit;
        *(const char**)(W.locale + 0x28) = punit;
        const float big[5] = {1e30f, -3.4e38f, 123456789.0f, 2e20f, -7e37f};
        for (int i = 0; i < 5; i++) { v->stat[i] = big[i]; v->stat2[i] = -big[4 - i]; }
        v->istat = 2000000000;
        strcpy(v->text, "Twenty characters ok");
        const uint32_t vt = U(v->vtbl);
        mem_save(g_snap);
        r = fx_run(f, true, {U(cc), 0});
        fx_check(fx_clean(f, r) && U(v->vtbl) == vt, "CarChooser::update_car, huge stats and 300-character units: a clean return, the viewer's vtable kept", &r);
        static char want[0x1000];
        const struct { const char* field; uint32_t size; } fields[] = {{cc->s18, 16}, {cc->s28, 16}, {cc->s38, 16}, {cc->s48, 16}, {cc->s58, 16},
                                                                      {cc->s68, 32}, {cc->s88, 16}, {cc->s98, 32}, {cc->sb8, 32}, {cc->sd8, 16}};
        for (int k = 0; k < 10; k++) {
            switch (k) {
            case 0: case 1: case 2: sprintf(want, "%3.1f %s", (double)v->stat[k], unit); break;
            case 3: case 4: sprintf(want, "%1.0f %s", 1.0 * (double)v->stat[k], lunit); break;
            case 5: strcpy(want, v->text); break;
            case 6: sprintf(want, "%d %s", (int)2000000000, punit); break;
            case 7: sprintf(want, "%1.0f %s@%1.0f %s", (double)v->stat2[0], unit, (double)v->stat2[1], unit); break;
            case 8: sprintf(want, "%1.0f %s@%1.0f %s", (double)v->stat2[2], unit, (double)v->stat2[3], unit); break;
            case 9: sprintf(want, "%1.0f %s", (double)v->stat2[4], unit); break;
            }
            want[fields[k].size - 1] = 0;
            sprintf(m, "CarChooser::update_car, huge stats and 300-character units: text %d is the first %u characters (\"%s\" / \"%.40s\")", k,
                    fields[k].size - 1, want, fields[k].field);
            fx_check(!strcmp(fields[k].field, want), m);
        }
        o = fx_outside(g_snap, {{cc, offsetof(CarChooser, viewer)}, {W.car_name, 0x80}, {(void*)(uintptr_t)0x0057b8c0, 12},
                                {(void*)(uintptr_t)0x0057b998, 12}, {(void*)(uintptr_t)0x0057bc28, 12}, {(void*)(uintptr_t)0x0057bc48, 12}});
        fx_check(o == 0, "CarChooser::update_car, huge stats and 300-character units: nothing written past the texts", 0, o);
        fx_reset();                                         // the first text exactly 15 characters, the others ordinary
        fx_xl(0x0057b8c0, "s", true);
        fx_xl(0x0057b998, "bhp", true);
        fx_xl(0x0057bc28, "rpm", true);
        fx_xl(0x0057bc48, "lb-ft", true);
        v->stat[0] = 12345678848.0f;
        for (int i = 1; i < 5; i++) { v->stat[i] = 100.0f; v->stat2[i] = 250.0f; }
        v->stat2[0] = 250.0f;
        v->istat = 300;
        strcpy(v->text, "ok");
        fx_same(f, {U(cc), 0}, "CarChooser::update_car, a 15-character text (the longest that fits)");
    }
    // ---- 11. RaceOptionViewer::Callback: the summary and the laps text ----
    {
        const Ent& f = fx_fn("RaceOptionViewer::Callback");
        RaceOptionViewer* rv = W.rov;
        const Span heap = {g_arena + A_HEAP, ARENA_BYTES - A_HEAP};   // the window's widgets (the groups shown and hidden)
        for (int ev = 0; ev < 2; ev++) {
            fx_reset();
            W.flags[1] = (uint8_t)(ev ? 0 : 1);
            W.opts->field = 1;
            W.opts->laps = (int32_t)0x80000000u;
            W.opts->event = 3;
            g_fx_enum[6] = mk(0, 'F', 100);
            g_fx_enum[7] = mk(1, 'E', 100);
            g_fx_xl = mk(2, 'L', 100);
            fx_xl(0x0057be88, 0, false);
            memset(W.summary, 'Z', 0x100);
            mem_save(g_snap);
            r = fx_run(f, true, {U(rv), 0, 0, U(&W.opts->field)});
            static char want[0x400], want2[0x400];
            if (ev) strcpy(want, g_fx_enum[7]);
            else sprintf(want, "%s: %d %s", g_fx_enum[6], (int)0x80000000u, g_fx_xl);
            want[0x1f] = 0;
            sprintf(want2, "%d %s", (int)0x80000000u, g_fx_xl);
            want2[0x23] = 0;
            bool ok = fx_clean(f, r) && !strcmp(W.summary, want);
            for (int i = 0x20; i < 0x100; i++) ok &= W.summary[i] == 'Z';
            fx_check(ok, ev ? "RaceOptionViewer::Callback, a 100-character event: the summary its first 31 characters, the rest of the caller's untouched"
                            : "RaceOptionViewer::Callback, a 100-character field and unit: the summary its first 31 characters, the rest of the caller's untouched",
                     &r);
            if (!ev) fx_check(!strcmp(rv->text, want2), "RaceOptionViewer::Callback, the least lap count and a 100-character unit: the laps text its first 35");
            o = fx_outside(g_snap, {{rv, sizeof(RaceOptionViewer)}, {W.summary, 0x20}, {W.opts, sizeof(GameOptions)},
                                    {(void*)(uintptr_t)0x0057be88, 12}, heap});
            fx_check(o == 0, ev ? "RaceOptionViewer::Callback, a 100-character event: nothing written past the summary"
                                : "RaceOptionViewer::Callback, a 100-character field and unit: nothing written past the texts", 0, o);
        }
        fx_reset();                                         // "<22>: 12 Laps": 31 characters
        W.flags[1] = 1;
        W.opts->field = 1;
        W.opts->laps = 12;
        g_fx_enum[6] = mk(0, 'F', 22);
        fx_xl(0x0057be88, "Laps", true);
        fx_same(f, {U(rv), 0, 0, U(&W.opts->field)}, "RaceOptionViewer::Callback, a 31-character summary (the longest that fits)");
        fx_reset();                                         // "F: -2147483648 <16 L>" 31, "-2147483648 <16 L>" 28 (the unit is
        W.flags[1] = 1;                                     // shared: the laps text can't reach 35 while the summary fits)
        W.opts->field = 1;
        W.opts->laps = (int32_t)0x80000000u;
        g_fx_enum[6] = mk(0, 'F', 1);
        fx_xl(0x0057be88, mk(1, 'L', 16), true);
        fx_same(f, {U(rv), 0, 0, U(&W.opts->field)}, "RaceOptionViewer::Callback, the least lap count, a 31-character summary");
        fx_reset();
        W.flags[1] = 0;
        W.opts->event = 3;
        g_fx_enum[7] = mk(0, 'E', 31);
        fx_same(f, {U(rv), 0, 0, U(&W.opts->field)}, "RaceOptionViewer::Callback, a 31-character event (the longest that fits)");
    }
    // ---- 12. BoardDo: a long title ----
    {
        const Ent& f = fx_fn("BoardDo");
        fx_reset();
        g_fx_enum[1] = mk(0, 'R', 50);
        g_fx_enum[0] = mk(1, 'S', 50);
        HS->script[0][0] = OP_NOP; HS->script[1][0] = OP_NOP;
        HS->script[2][0] = OP_KEY; HS->script[2][1] = 0x1b;
        HS->script_n = 3;
        g_rec_mark = 'R';
        g_rec_fprf[0] = 0;
        r = fx_run(f, true, {U("bemidji"), 1, 2, 0});
        g_rec_mark = 0;
        fx_check(fx_clean(f, r), "BoardDo, a 102-character title: a clean return", &r);
        char want[0x80];
        sprintf(want, "%s: %s", g_fx_enum[1], g_fx_enum[0]);
        want[0x3f] = 0;
        sprintf(m, "BoardDo, a 102-character title: its first 63 characters shown (\"%.70s\")", g_rec_fprf);
        fx_check(!strcmp(g_rec_fprf, want), m);
        fx_reset();
        g_fx_enum[1] = mk(0, 'R', 30);
        g_fx_enum[0] = mk(1, 'S', 31);
        HS->script[0][0] = OP_NOP; HS->script[1][0] = OP_NOP;
        HS->script[2][0] = OP_KEY; HS->script[2][1] = 0x1b;
        HS->script_n = 3;
        fx_same(f, {U("bemidji"), 1, 2, 0}, "BoardDo, a 63-character title (the longest that fits)");
    }
    // ---- 13. BoardCustomText::Draw: a text that reads arguments as a format ----
    {
        const Ent& f = fx_fn("BoardCustomText::Draw");
        static const char* const reads[] = {"%d laps", "%s", "%n", "Race: %*d", "%.*f", "%5.2s", "%I64d", "%hB", "100%% %c", "%lx", "% +#08.3e",
                                            "%%%p", "Arcade: 5%s"};
        static const char* const plain[] = {"50%% off", "100%", "%%%%", "", "%I6x", "%-5", "%5.3 b", "%k", "%.", "%l", "Arcade: Simulation",
                                            "a%%b%", "%I"};
        for (const char* t : reads) {
            fx_reset();
            W.bct->text = t;
            mem_save(g_snap);
            g_rec_fprf_fmt = 0;
            r = fx_run(f, true, {U(W.bct), 0, U(W.screen)});
            sprintf(m, "BoardCustomText::Draw, \"%s\": printed as it is (\"%%s\", the text)", t);
            fx_check(fx_clean(f, r) && U(g_rec_fprf_fmt) == 0x004f1e84u && g_rec_fprf_a0 == U(t), m, &r);
            o = fx_outside(g_snap, {{W.screen, sizeof(gxCanvas)}, {(void*)(uintptr_t)S_GX_CANVAS, 4}});
            sprintf(m, "BoardCustomText::Draw, \"%s\": nothing written but the canvas", t);
            fx_check(o == 0, m, 0, o);
        }
        for (const char* t : plain) {
            fx_reset();
            W.bct->text = t;
            g_rec_fprf_fmt = 0;
            r = fx_run(f, true, {U(W.bct), 0, U(W.screen)});
            sprintf(m, "BoardCustomText::Draw, \"%s\": the format, as the original", t);
            fx_check(fx_clean(f, r) && g_rec_fprf_fmt == t, m, &r);
        }
        for (const char* t : {"50%% off", "100%", "%%%%", "", "a%%b%"}) {
            fx_reset();
            W.bct->text = t;
            sprintf(m, "BoardCustomText::Draw, \"%s\"", t);
            fx_same(f, {U(W.bct), 0, U(W.screen)}, m);
        }
        // the check against the game's printf: the test texts ('B' aside: it reads the stale locals, not an argument) and
        // 20000 random ones
        g_noaccess = (uint8_t*)VirtualAlloc(0, 0x1000, MEM_COMMIT, PAGE_NOACCESS);
        fx_reset();
        int agree = 0, n = 0;
        auto cmp = [&](const char* t) {
            const bool a = menu_board::fix_format_reads_args(t), b = game_reads_args(t);
            n++;
            agree += a == b;
            if (a != b) printf("  the format check says %s, the game's printf %s: \"%s\"\n", a ? "reads" : "doesn't read", b ? "reads" : "doesn't", t);
        };
        for (const char* t : reads) if (!strchr(t, 'B')) cmp(t);
        for (const char* t : plain) cmp(t);
        static const char alpha[] = "%%%%%*.-+ #0123456789lhILNFsdxkq64cS";
        char t[24];
        for (int i = 0; i < 20000; i++) {
            const int len = irange(1, 12);
            for (int k = 0; k < len; k++) t[k] = alpha[rnd() % (sizeof alpha - 1)];
            t[len] = 0;
            cmp(t);
        }
        sprintf(m, "the board text's format check agrees with the game's printf (%d of %d)", agree, n);
        fx_check(agree == n, m);
        fx_check(menu_board::fix_format_reads_args("%B") && menu_board::fix_format_reads_args("%-5B"), "the format check takes a 'B' conversion as reading");
        VirtualFree(g_noaccess, 0, MEM_RELEASE);
    }
    mem_load(g_pristine);
    printf("directed fix tests: %s -- %d checks, %d failed (%d boundary cases compared with the original)\n", g_fx_bad ? "FAILED" : "all passed",
           g_fx_n, g_fx_bad, g_fx_same_n);
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
    char* s = strstr(exe, "\\test\\world_menu_view.cpp");
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
        {F_LogPanic, (void*)&stub_LogPanic}, {F_SingleBegin, (void*)&stub_SingleBegin}, {F_SingleEnd, (void*)&stub_SingleEnd},
        {F_atexit, (void*)&stub_atexit}, {F_PTimeNow, (void*)&stub_PTimeNow}, {F_MouseGetEvent, (void*)&stub_MouseGetEvent},
        {F_KeyHit, (void*)&stub_KeyHit}, {F_KeyGet, (void*)&stub_KeyGet}, {F_MousePeek, (void*)&stub_MousePeek},
        {F_MouseClear, (void*)&stub_MouseClear}, {F_KeyClear, (void*)&stub_KeyClear},
        {F_gxGetStamp, (void*)&stub_gxGetStamp}, {F_gxForgetStamp, (void*)&stub_gxForgetStamp},
        {F_gxStampWidth, (void*)&stub_gxStampWidth}, {F_gxStampHeight, (void*)&stub_gxStampHeight},
        {F_gxStampCount, (void*)&stub_gxStampCount}, {F_gxStampHotSpot, (void*)&stub_gxStampHotSpot},
        {F_gxStampHitTest, (void*)&stub_gxStampHitTest}, {F_gxDrawStamp, (void*)&stub_gxDrawStamp},
        {F_gxFontGet, (void*)&stub_gxFontGet}, {F_gxFontForget, (void*)&stub_gxFontForget},
        {F_gxFontHeight, (void*)&stub_gxFontHeight}, {F_gxFontAscent, (void*)&stub_gxFontAscent},
        {F_gxFontDescent, (void*)&stub_gxFontDescent}, {F_gxFontStringWidth, (void*)&stub_gxFontStringWidth},
        {F_gxFontStringWidthN, (void*)&stub_gxFontStringWidthN}, {F_gxFontPrintN, (void*)&stub_gxFontPrintN},
        {F_gxFontPrintf, (void*)&stub_gxFontPrintf}, {F_gxPaletteCreate, (void*)&stub_gxPaletteCreate},
        {F_gxPaletteDestroy, (void*)&stub_gxPaletteDestroy}, {F_gxPaletteMakeGradient, (void*)&stub_gxPaletteMakeGradient},
        {F_gxSetCanvas, (void*)&stub_gxSetCanvas}, {F_gxClear, (void*)&stub_gxClear}, {F_gxRect, (void*)&stub_gxRect},
        {F_gxLine, (void*)&stub_gxLine}, {F_gxPaste, (void*)&stub_gxPaste}, {F_gxText, (void*)&stub_gxText},
        {F_gxTextHeight, (void*)&stub_gxTextHeight}, {F_gxAllocCanvas, (void*)&stub_gxAllocCanvas},
        {F_gxFreeCanvas, (void*)&stub_gxFreeCanvas}, {F_gxGrabScreen, (void*)&stub_gxGrabScreen},
        {F_gxReleaseScreen, (void*)&stub_gxReleaseScreen}, {F_gxFlip, (void*)&stub_gxFlip},
        {F_mrBeginFrame, (void*)&stub_mrBeginFrame}, {F_mrEndFrame, (void*)&stub_mrEndFrame},
        {F_Xlator_xlate, (void*)&stub_xlate}, {F_sprintf, (void*)&stub_sprintf},
        {F_LocaleConvertNumeric, (void*)&stub_LocaleConvertNumeric}, {F_tolower, (void*)&stub_tolower},
        {F_ControlToDisplayString, (void*)&stub_ControlToDisplayString}, {F_ControlUpdate, (void*)&stub_ControlUpdate},
        {F_ControlDetectInit, (void*)&stub_ControlDetectInit}, {F_ControlDetect, (void*)&stub_ControlDetect},
        {F_FileFindFirst, (void*)&stub_FileFindFirst}, {F_FileFindNext, (void*)&stub_FileFindNext},
        {F_FileFindClose, (void*)&stub_FileFindClose}, {F_FileOpen, (void*)&stub_FileOpen}, {F_FileClose, (void*)&stub_FileClose},
        {F_ClipboardGetText, (void*)&stub_ClipboardGetText},
        // this group's callees
        {F_OptionsGet, (void*)&stub_OptionsGet}, {F_OptionsSet, (void*)&stub_OptionsSet},
        {F_GetTrackCount, (void*)&stub_GetTrackCount}, {F_GetTrackName, (void*)&stub_GetTrackName},
        {F_GetTrackFriendlyName, (void*)&stub_GetTrackFriendlyName}, {F_GetTrackText, (void*)&stub_GetTrackText},
        {F_GetTrackNumber, (void*)&stub_GetTrackNumber}, {F_GetTrackDifficulty, (void*)&stub_GetTrackDifficulty},
        {F_GetLapCountFromType, (void*)&stub_GetLapCountFromType},
        {F_GetRealismString, (void*)&stub_enum_string<0>}, {F_GetRaceTypeString, (void*)&stub_enum_string<1>},
        {F_GetGhostCarTypeString, (void*)&stub_enum_string<2>}, {F_GetWeatherString, (void*)&stub_enum_string<3>},
        {F_GetGameTimeString, (void*)&stub_enum_string<4>}, {F_GetAIStrengthString, (void*)&stub_enum_string<5>},
        {F_GetFieldString, (void*)&stub_enum_string<6>}, {F_GetEventString, (void*)&stub_enum_string<7>},
        {F_ResourceSetMustLoad, (void*)&stub_ResourceSetMustLoad}, {F_ResourceSetUnload, (void*)&stub_ResourceSetUnload},
        {F_LocaleFormatShortDate, (void*)&stub_LocaleFormatShortDate},
        {F_StringTableGet, (void*)&stub_StringTableGet}, {F_StringTableForget, (void*)&stub_StringTableForget},
        {F_StringTableGetEntry, (void*)&stub_StringTableGetEntry},
        {F_RecordMgrCreate, (void*)&stub_RecordMgrCreate}, {F_RecordMgrDestroy, (void*)&stub_RecordMgrDestroy},
        {F_RecordMgr_Clear, (void*)&stub_RecordMgr_Clear}, {F_RecordMgr_SetMode, (void*)&stub_RecordMgr_SetMode},
        {F_RecordMgr_GetNthBestRace, (void*)&stub_GetNth<0>}, {F_RecordMgr_GetNthBestLap, (void*)&stub_GetNth<1>},
        {F_RecordMgr_GetNthBestSpeed, (void*)&stub_GetNth<2>}, {F_PhysicsTimeString, (void*)&stub_PhysicsTimeString},
        {F_mrSetView, (void*)&stub_mrSetView}, {F_mrSetProjection, (void*)&stub_mrSetProjection},
        {F_mrSetCamera, (void*)&stub_mrSetCamera}, {F_mrEnable, (void*)&stub_mrEnable}, {F_mrDisable, (void*)&stub_mrDisable},
        {F_mrModelEnvMap, (void*)&stub_mrModelEnvMap}, {F_mrModelSetAlpha, (void*)&stub_mrModelSetAlpha},
        {F_mrModelClearAlpha, (void*)&stub_mrModelClearAlpha}, {F_mrModelDraw, (void*)&stub_mrModelDraw},
        {F_mrModelLoad, (void*)&stub_mrModelLoad}, {F_mrModelLoadRemap, (void*)&stub_mrModelLoadRemap},
        {F_mrModelUnload, (void*)&stub_mrModelUnload}, {F_mrModelRemapTextures, (void*)&stub_mrModelRemapTextures},
        {F_mrModelCopyTransform, (void*)&stub_mrModelCopyTransform}, {F_mrModelDestroyCopy, (void*)&stub_mrModelDestroyCopy},
        {F_mrModelGetExtents, (void*)&stub_mrModelGetExtents}, {F_WorldGetCarTexture, (void*)&stub_WorldGetCarTexture},
        {F_CarFileLoad, (void*)&stub_CarFileLoad}, {F_PaintKitDo, (void*)&stub_PaintKitDo},
        {F_stricmp, (void*)&stub_stricmp}, {F_atof, (void*)&stub_atof}, {F_atoi, (void*)&stub_atoi},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    build_world();
    mem_save(g_pristine);
    printf("world: %u $E initialisers run, %d widgets in the window, %u KB of heap used\n", (unsigned)g_setup_e.size(),
           (int)W.widgets.size(), (HS->heap_next - A_HEAP) / 1024);

    int dup = 0;
#if MENU_FIXES
    const int fix_bad = directed_fix_tests();
#else
    const int fix_bad = 0;
#endif
    for (int i = 0; i < g_nfns; i++)
        for (int j = i + 1; j < g_nfns; j++)
            if (g_fns[i].v10 == g_fns[j].v10 || !strcmp(g_fns[i].name, g_fns[j].name)) {
                printf("  listed twice: %08x %s / %08x %s\n", g_fns[i].v10, g_fns[i].name, g_fns[j].v10, g_fns[j].name);
                dup++;
            }

    static Footprint fp;
    long long checks = 0, poisoned_checks = 0, log_words = 0;
    int differ = 0, fp_bad = 0, faults = 0, pure_n = 0, replay_n = 0, changed_fns = 0, still_fns = 0, bad_fns = 0, skipped = 0;
    long long both_fault = 0, orig_only = 0, overran = 0;
    char fixed_fns[2048] = "";
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0, fn_fpck = 0, fn_orig_only = 0, fn_overran = 0;
        const int nr = is_modal(f.name) ? rounds : rounds * 10;
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            if (poisoned) poison(g_pristine);
            randomize_world();
            randomize_statics();
            random_script(is_modal(f.name) ? 14 : 4);
            uint32_t words[72];
            if (!make_args(f, words)) { skipped++; continue; }
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            mem_save(g_snap);
            g_overran = false;
            const Result ro = run(f, false, words);
            const bool ro_overran = g_overran;
            const bool changed = mem_diff(g_snap) != 0;
            fn_changed |= changed;
            if (!fp.replay_only && !ro.fault) {
                fn_fpck++;
                uint32_t out = 0;
                auto covered = [&](const uint8_t* p) {
                    for (int k = 0; k < fp.n; k++) if (p >= (uint8_t*)fp.r[k].p && p < (uint8_t*)fp.r[k].p + fp.r[k].n) return true;
                    return false;
                };
                for (uint32_t i2 = 0; i2 < DATA_BYTES && !out; i2++)
                    if (g_snap.data[i2] != DATA[i2] && (fp.pure || !covered(DATA + i2))) out = 0x004e1000 + i2;
                for (uint32_t i2 = sizeof(HState); i2 < ARENA_BYTES && !out; i2++)
                    if (g_snap.arena[i2] != g_arena[i2] && (fp.pure || !covered(g_arena + i2))) out = U(g_arena + i2);
                if (out) {
                    printf("  FOOTPRINT %08x %s: the original changed %08x outside it (round %d)\n", f.v10, f.name, out, rd);
                    fp_bad++;
                    fn_bad = true;
                }
            }
            mem_save(g_after);
            memcpy(&g_log_orig, &g_log, sizeof(uint32_t) * (1 + (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)));
            mem_load(g_snap);
            const Result rn = run(f, true, words);
            if (MENU_FIXES && ((ro.fault && !rn.fault) || ro_overran)) {   // a fixed case: not compared (counted)
                if (ro.fault && !rn.fault) { orig_only++; fn_orig_only++; }
                else { overran++; fn_overran++; }
                if (rd == 0) { pure_n += fp.pure; replay_n += fp.replay_only != 0; }
                continue;
            }
            checks++;
            fn_checks++;
            poisoned_checks += poisoned;
            log_words += g_log_orig.n;
            if (ro.fault || rn.fault) { faults++; fn_faults++; }
            if (ro.fault && rn.fault) both_fault++;
            bool same = ro.fault == rn.fault && ro.code == rn.code && ro.ret == rn.ret && ro.pops == rn.pops && ro.top == rn.top &&
                        !memcmp(&ro.st0, &rn.st0, sizeof ro.st0) && !memcmp(ro.regs, rn.regs, sizeof ro.regs);
            const uint32_t where = mem_diff(g_after);
            same &= where == 0;
            same &= g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
            if (!same) {
                printf("  MISMATCH %08x %s (round %d, %s, pc %s)\n", f.v10, f.name, rd, poisoned ? "poisoned" : "as loaded", g_pc == _PC_24 ? "24" : "53");
                printf("    fault %d/%d (%08x at %08x / %08x at %08x), return %08x / %08x, st0 %g / %g, x87 top %u / %u, popped %u / %u,"
                       " ebx esi edi ebp %08x %08x %08x %08x / %08x %08x %08x %08x\n",
                       ro.fault, rn.fault, ro.code, ro.eip, rn.code, rn.eip, ro.ret, rn.ret, ro.st0, rn.st0, ro.top, rn.top, ro.pops, rn.pops,
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
        if (fn_orig_only || fn_overran) {
            char e[128];
            sprintf(e, "%s%s %d/%d", fixed_fns[0] ? ", " : "", f.name, fn_orig_only, fn_overran);
            if (strlen(fixed_fns) + strlen(e) < sizeof fixed_fns) strcat(fixed_fns, e);
        }
        if (trace)
            printf("%08x %-52s %s%s (%d checks, %d with the footprint checked, %d faulted)\n", f.v10, f.name, fn_bad ? "BAD" : "ok",
                   fn_changed ? "" : " (never changed memory)", fn_checks, fn_fpck, fn_faults);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    printf("%d functions (%d pure, %d replay_only in round 0, %d others), %d listed twice; %lld checks (%lld on poisoned .data), "
           "%lld logged words compared: %d differ, %d footprint violations, %d checks faulted (%lld in both, the same way), %d skipped; "
           "%d functions bad\n", g_nfns, pure_n, replay_n, g_nfns - pure_n - replay_n, dup, checks, poisoned_checks, log_words, differ,
           fp_bad, faults, both_fault, skipped, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    if (MENU_FIXES)
        printf("fix build: %lld rounds where only the original faulted and %lld where its sprintf printed past a field, kept out of "
               "the comparison (per function: faulted/printed past): %s\n", orig_only, overran, fixed_fns[0] ? fixed_fns : "none");
    return differ || fp_bad || dup || fix_bad ? 1 : 0;
}
