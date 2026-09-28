// world_ui.cpp -- M3 UI stage, step U1: every rewrite of library `ui` (hook/ui_style.cpp, ui_window.cpp,
// ui_widget.cpp, ui_dialog.cpp and the generated ui_leftover.cpp) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_ui.cpp
//        /Fo<dir>\ /Fe<dir>\world_ui.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_ui.exe [rounds] [seed]          (VP_TRACE=1: one line per function)
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the five rewrite files, with PORT_FN
// redefined to list each function: its v1.0 address, the rewrite, its calling convention, stack arguments (in dwords)
// and return (width, or float in st0), and its footprint.
//
// The world. Once, at start: the game's own UIBegin (the style table from its 24 descriptions: fonts, palettes and
// stamps come from the stubs below), then three windows made by the game's WidgetCreateWindow and _UIAddItems from
// item lists covering all 23 item types (buttons with and without callbacks and Escape / Return keys, bitmap buttons,
// hot keys, text, numbers, cycling strings, text fields of one and several lines, a control detector, check boxes, radio
// buttons plain and in group mode, stamp rolls, sliders with their arrows, arrows, scroll bars both ways, scroll buttons,
// list boxes, drop lists, rectangles, a CustomWidget on a fake UICustomControl whose every virtual logs, groups), a
// title bar and a menu button added by hand, the values they watch, string lists, scroll axes, canvases; one window
// running. This is the pristine state.
//
// Then for EVERY listed function, `rounds` times (default 24; ten times that for a function that doesn't run a window
// or a static initialiser), the widget a method runs on pushed towards the states its branches need: pristine restored; half the rounds POISONED (every byte
// of .data/.bss random, except what's read as a string or followed as a pointer: the library's strings, the window
// table and the running window, the current canvas, the Xlators, the file boxes' pointers, __adjust_fdiv); the widgets'
// states random (over / focus / visible / enabled / dirty / pressed flags, timers, the values they edit -- floats
// sometimes NaN / infinite / huge --, selections and scroll positions, text in the text fields and the caret, the window's
// captured / over / focus widgets, hidden / disabled groups); arguments made for the function (the right class of widget
// for a method, mouse positions on and off it, key codes and characters, style strings with spaces and newlines,
// canvases, fresh blocks for constructors); for a function that runs a window modally (the dialogs, UIDoMenu,
// WidgetExecuteWindow, CDetect::MouseUp...) a random input script: mouse moves onto the running window's widgets, presses
// and releases, keys (Escape, Return, space, Tab arrows, Home / End / arrows / Delete / Backspace, letters, Ctrl-V), idle
// frames -- ended, if the dialog doesn't end itself, by a watchdog after 40 frames. Snapshot; the stack below is filled
// with one pattern (so a local the original leaves uninitialised reads the same in both passes); the ORIGINAL runs from a
// raw call thunk (ecx / edx / the stack set exactly; eax / edx / st0, the x87 stack depth, the bytes popped, ebx / esi /
// edi / ebp recorded); its results kept, the snapshot restored, the REWRITE runs the same way. Compared: every byte of
// .data/.bss/.idata and the 1 MB arena (a dword where both passes left a pointer into this thread's stack counts as equal:
// each pass's frames differ), the return value, the call logs of every stub, faults. Every byte the original changed must
// lie in the rewrite's footprint (unless it's replay_only; the stubs' own state block excepted), a pure one changes
// nothing. The x87 runs at 24 or 53 bits (alternating rounds).
//
// Stubbed (a jump to a logger; state kept in the arena so both passes see the same): MemAlloc (a bump heap in the arena)
// and operator delete; LogReport / LogPanic; the input (MouseGetEvent / KeyHit / KeyGet / MousePeek / MouseClear /
// KeyClear, from the script), PTimeNow (16 ms a call, and the watchdog); SingleBegin / _SingleEnd; the 2D calls that
// draw or measure (gxDrawStamp, gxClear, gxRect, gxLine, gxPaste, gxText, gxFontPrintN / Printf -- each logs its
// arguments, the current canvas and its clip rectangle --, the stamps' and fonts' sizes, deterministic from their names),
// gxSetCanvas, gxGetStamp / gxForgetStamp, gxFontGet / Forget, the palettes, gxAllocCanvas / gxFreeCanvas,
// gxGrabScreen / gxReleaseScreen / gxFlip, mrBeginFrame / mrEndFrame; Xlator::xlate (the key as the text); sprintf
// (this program's, its raw argument dwords logged), LocaleConvertNumeric, tolower; the controls (ControlUpdate /
// DetectInit / Detect / ToDisplayString), the files (FileFindFirst / Next / Close, FileOpen / Close), the clipboard,
// atexit, and the game's CRT fatal paths. The game's own code runs everywhere else: gxSetClip / gxRestoreClip,
// strncpy / strchr / strrchr / memmove / __ftol, UIDialogItem's and Xlator's constructors, and every function of this
// library (each rewrite is checked against its original with the same callees).
#define _CRT_SECURE_NO_WARNINGS
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
static Ent g_fns[1024];
static int g_nfns;
struct EntReg { EntReg(const Ent& e) { if (g_nfns < 1024) g_fns[g_nfns++] = e; } };
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

#include "../hook/ui_style.cpp"
#include "../hook/ui_window.cpp"
#include "../hook/ui_widget.cpp"
#include "../hook/ui_dialog.cpp"
#include "../hook/ui_leftover.cpp"

using namespace uit;

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
    case 0: return bitsf(0x7fc00000u);
    case 1: return (rnd() & 1) ? INFINITY : -INFINITY;
    case 2: return 0.0f;
    case 3: return range(-1e30f, 1e30f);
    case 4: return bitsf(rnd() & 0x807fffff);
    default: return range(-100.0f, 100.0f);
    }
}

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
// the first difference between saved and live memory at or after byte i of one block, as an index (n: none); a dword
// where both hold a pointer into this thread's stack counts as equal
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
    int32_t cb_calls, idle_calls, detect_calls, detect_after, idle_exit_after, cb_mask;
    int32_t grab_fail_at;
    int32_t pal_next, alloc_canvas_next;
    int32_t nfiles, file_pos;
    char files[8][24];
    int32_t file_open_mask;
};
#define HS ((HState*)g_arena)
enum : uint32_t { A_HEAP = 0x60000 };                     // the bump heap: arena + 0x60000 .. the end

// ---- the log ----------------------------------------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 16 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void L(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static uintptr_t g_self_lo, g_self_hi;                    // this program's image
static bool readable(const void* p, uint32_t n) {
    const uintptr_t a = (uintptr_t)p;
    if (a >= (uintptr_t)g_arena && a + n <= (uintptr_t)g_arena + ARENA_BYTES) return true;
    if (a >= 0x401000 && a + n <= 0x5d9000) return true;
    if (a >= g_self_lo && a + n <= g_self_hi) return true;
    if (on_stack((uint32_t)a) && on_stack((uint32_t)(a + n - 1))) return true;
    return false;
}
// a pointer as logged: 'STAK' for this thread's stack (each pass's frames differ), else its value
static uint32_t P(const void* p) { return on_stack((uint32_t)(uintptr_t)p) ? 'STAK' : (uint32_t)(uintptr_t)p; }
static void LS(const char* s, int max = 0x400) {         // a string's bytes (or its pointer if it can't be read)
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
static void LN(const char* s, int n) {                    // n bytes of a string (as gxFontPrintN draws them)
    if (n < 0 || n > 0x1000) { L('NBAD'); L((uint32_t)n); return; }
    if (!readable(s, (uint32_t)(n ? n : 1))) { L('BADS'); return; }
    uint32_t w = 0;
    for (int i = 0; i < n; i++) { w = w << 8 | (uint8_t)s[i]; if ((i & 3) == 3) { L(w); w = 0; } }
    L(w);
    L((uint32_t)n);
}
static void L_canvas() {                                  // the current canvas and its clip, where a draw lands
    const gxCanvas* c = UI_GP(gxCanvas, S_GX_CANVAS);
    L(P(c));
    if (c && readable(c, sizeof(gxCanvas))) { L((uint32_t)c->cx0); L((uint32_t)c->cy0); L((uint32_t)c->cx1); L((uint32_t)c->cy1); }
}

// ---- stubs: memory, logs, sync ------------------------------------------------------------------------------------------------
static void* __cdecl stub_MemAlloc(int n) {
    L('ALOC'); L((uint32_t)n);
    uint32_t a = (HS->heap_next + 7) & ~7u;
    if (a < A_HEAP || a + (uint32_t)(n > 0 ? n : 0) > ARENA_BYTES - 0x100 || n < 0 || n > 0x40000) { L('OOM '); return 0; }
    HS->heap_next = a + (uint32_t)n;
    return g_arena + a;
}
static void __cdecl stub_delete(void* p) { L('DEL '); L(P(p)); }
// a printf-style call's arguments: as many dwords as the format's conversions take (a double two, '*' one)
static void L_va(const char* fmt, const uint32_t* w) {
    L((uint32_t)(uintptr_t)fmt);
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
        if (op[0] == OP_KEY) return 0;                    // the key goes first (KeyHit)
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

// ---- stubs: the 2D calls -------------------------------------------------------------------------------------------------------
// fake stamps and fonts: a static table keyed by name, so a name always gives the same pointer
struct Fake { uint32_t magic; char name[32]; int32_t w, h, count, hx, hy, asc, desc, cw; };
static Fake g_fakes[64];
static int g_nfakes;
static uint32_t hash_str(const char* s) { uint32_t h = 2166136261u; for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }
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
static int32_t fake_val(const void* p, int which) {     // a stamp's or font's measure; deterministic for an unknown pointer
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
    L('FPRF'); L(P(f)); L(P(pal)); L(flags); L((uint32_t)x); L((uint32_t)y); LS(fmt); L_canvas();
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

// ---- stubs: text, controls, files ------------------------------------------------------------------------------------------------
static void __fastcall stub_xlate(uint32_t* xl, int) {
    L('XLAT'); L(P(xl));
    xl[1] = xl[0];
    xl[2] = UI_GU32(S_XLATOR_COOKIE);
}
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
        nw += strchr("eEfgG", *p) ? 2 : 1;
    }
    for (int i = 0; i < nw && i < 16; i++) {
        uint32_t v = w[i];
        L(on_stack(v) ? 'STAK' : v);
    }
    const int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    LS(buf);
    return r;
}
static void __cdecl stub_LocaleConvertNumeric(char* s) { L('LCNV'); LS(s); }
static int __cdecl stub_tolower(int c) { L('TLOW'); L((uint32_t)c); return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static void __cdecl stub_ControlToDisplayString(char* buf, int n, const uint32_t* ctl) {
    L('CTDS'); L(P(buf)); L((uint32_t)n); L(P(ctl));
    char t[32];
    sprintf(t, "C%08x", readable(ctl, 4) ? ctl[0] : 0u);
    if (n > 0) { strncpy(buf, t, (size_t)n - 1); buf[n - 1] = 0; }
}
static void __cdecl stub_ControlUpdate(uint8_t u) { L('CUPD'); L(u); }
static void __cdecl stub_ControlDetectInit() { L('CDIN'); }
static uint8_t __cdecl stub_ControlDetect(uint32_t* ctl) {
    L('CDET'); L(P(ctl));
    if (++HS->detect_calls < HS->detect_after) return 0;
    if (readable(ctl, 8)) { ctl[0] = 0x1234 + (uint32_t)HS->detect_calls; ctl[1] = 7; }
    return 1;
}
static void* __cdecl stub_FileFindFirst(const char* pat, char* out, int n) {
    L('FFF '); LS(pat); L((uint32_t)n);
    HS->file_pos = 0;
    if (HS->nfiles <= 0) return (void*)(intptr_t)-1;
    strncpy(out, HS->files[0], (size_t)n); HS->file_pos = 1;
    return (void*)0x4f11e;
}
static uint8_t __cdecl stub_FileFindNext(void* h, char* out, int n) {
    L('FFN '); L((uint32_t)(uintptr_t)h);
    if (HS->file_pos >= HS->nfiles) return 0;
    strncpy(out, HS->files[HS->file_pos++], (size_t)n);
    return 1;
}
static void __cdecl stub_FileFindClose(void* h) { L('FFC '); L((uint32_t)(uintptr_t)h); }
static int32_t __cdecl stub_FileOpen(const char* name) {
    L('FOPN'); LS(name);
    return readable(name, 1) && ((hash_str(name) ^ (uint32_t)HS->file_open_mask) & 1) ? 5 : 0;
}
static void __cdecl stub_FileClose(int32_t* fd) { L('FCLS'); L((uint32_t)*fd); *fd = 0; }
static uint8_t __cdecl stub_ClipboardGetText(char* buf, int n) {
    L('CLIP'); L(P(buf)); L((uint32_t)n);
    if (n > 0) { strncpy(buf, "pasted text", (size_t)n - 1); buf[n - 1] = 0; }
    return 1;
}

// ---- the harness's own callbacks: button callbacks, idle functions, a UICustomControl -----------------------------------
static uint8_t __cdecl hb_callback(int id) { L('CB  '); L((uint32_t)id); return (uint8_t)(((uint32_t)HS->cb_calls++ ^ (uint32_t)HS->cb_mask) & 1); }
static uint8_t __cdecl hb_idle(int32_t* code) {
    L('IDLE'); L(*code);
    if (++HS->idle_calls >= HS->idle_exit_after) { *code = -5 - HS->idle_calls; return 1; }
    return 0;
}
static void __cdecl hb_window_idle() { L('WIDL'); }
template <int S> static void __fastcall cc_void(void* self, int) { L('CC  '); L(S); L(P(self)); }
template <int S> static void __fastcall cc_xy(void* self, int, int32_t x, int32_t y) { L('CC  '); L(S); L(P(self)); L((uint32_t)x); L((uint32_t)y); }
static void __fastcall cc_callback(void* self, int, int32_t id, const void* p) { L('CC  '); L(0x10); L(P(self)); L((uint32_t)id); L(P(p)); }
static void __fastcall cc_draw(void* self, int, gxCanvas* c) { L('CC  '); L(0x18); L(P(self)); L(P(c)); }
static void* __fastcall cc_cursor(void* self, int) { L('CC  '); L(0x44); L(P(self)); return fake_for("cc_cursor", 'STMP'); }
static uint8_t __fastcall cc_charhit(void* self, int, uint16_t k) { L('CC  '); L(0x48); L(P(self)); L(k); return (uint8_t)(k & 1); }
static uint8_t __fastcall cc_tabstop(void* self, int) { L('CC  '); L(0x4c); L(P(self)); return (uint8_t)(((uintptr_t)self >> 4) & 1); }
static void* __fastcall cc_dtor(void* self, int, uint32_t fl) { L('CC  '); L(0); L(P(self)); L(fl); return self; }
static void* g_cc_vtbl[20] = {
    (void*)&cc_dtor, (void*)&cc_void<4>, (void*)&cc_void<8>, (void*)&cc_void<0xc>, (void*)&cc_callback, (void*)&cc_void<0x14>,
    (void*)&cc_draw, (void*)&cc_void<0x1c>, (void*)&cc_xy<0x20>, (void*)&cc_xy<0x24>, (void*)&cc_xy<0x28>, (void*)&cc_xy<0x2c>,
    (void*)&cc_xy<0x30>, (void*)&cc_void<0x34>, (void*)&cc_void<0x38>, (void*)&cc_void<0x3c>, (void*)&cc_void<0x40>,
    (void*)&cc_cursor, (void*)&cc_charhit, (void*)&cc_tabstop,
};

// ---- the raw call ---------------------------------------------------------------------------------------------------------------
static uint32_t g_c_fn, g_c_ecx, g_c_edx, g_c_n, g_c_args[64];
static uint32_t g_r_eax, g_r_edx, g_r_ebx, g_r_esi, g_r_edi, g_r_ebp, g_r_esp, g_c_top, g_save_esp;
static uint16_t g_r_sw;
static double g_r_st0;
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
struct Result { int fault; uint32_t code, eip, ret, pops, regs[4], top; double st0; };
static uint32_t g_fault_code, g_fault_eip;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_eip = (uint32_t)e->ContextRecord->Eip;
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}
// the stack the call will use, filled with one pattern first (so a local the original leaves uninitialised reads the
// same thing in both passes)
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
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    __try {
        raw_call();
    } __except (fault_filter(GetExceptionInformation())) { r.fault = 1; r.code = g_fault_code; r.eip = g_fault_eip; }
    __asm fninit
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
static uint32_t g_wv;                                     // the world's own bump pointer (arena offset), below A_HEAP
static void* wv(uint32_t n) { g_wv = (g_wv + 7) & ~7u; void* p = g_arena + g_wv; g_wv += n; memset(p, 0, n); return p; }
static char* wstr(const char* s) { char* p = (char*)wv((uint32_t)strlen(s) + 1); strcpy(p, s); return p; }
static void call_orig(uint32_t fn, int fast, std::initializer_list<uint32_t> words) {
    Ent e = {fn, "(setup)", 0, fast, (int)words.size() - (fast ? 2 : 0), 4, 0, 0};
    uint32_t w[32];
    int i = 0;
    for (uint32_t x : words) w[i++] = x;
    g_pc = _PC_53;
    const Result r = run(e, false, w);
    if (r.fault) printf("setup: %08x faulted (%08x at %08x)\n", fn, r.code, r.eip);
}
#define U(p) ((uint32_t)(uintptr_t)(p))

struct World {
    gxCanvas* screen;
    WidgetWindow* win[3];
    UIStringList* list[3];
    UIScrollAxis* axis[3];
    UICustomControl* cc;
    uint32_t* group;                                  // the groups' bits (EnterGroup's out)
    uint8_t* bytes;                                   // check boxes' values
    int32_t* ints;                                    // radio / multi / roll / int numeric / selections
    float* floats;                                    // sliders, arrows, numerics
    uint32_t* controls;                               // CDetect's (8 bytes each)
    char* buf[3];                                     // text fields' buffers (0x100 each)
    int32_t buf_cap[3];
    char* texts[12];                                  // random strings (0x100 each), remade each round
    char* scratch;                                    // outputs, fresh blocks for constructors (0x1400)
    char* scratch2;
    UIDialogItem* items[3];
    int nitems[3];
    UIDialog* dlg;
    UIMenu* menu;
    UIStyleDesc* desc;
    char* name_buf;                                   // the file boxes' name (a world copy)
    uint8_t* fstatic;                                 // an FStaticText and a Decal (never built by the game)
    uint8_t* decal;
    gxCanvas* decal_canvas;
    std::vector<Widget*> widgets;                     // every widget of windows 0 and 1
};
static World W;

static UIDialogItem* it_(UIDialogItem* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h, const void* text,
                         int32_t i1c, const void* data, int32_t style, float lo = 0, float hi = 0, int32_t* sel = 0) {
    memset(it, 0, sizeof *it);
    it->type = type; it->id = id; it->x = x; it->y = y; it->w = w; it->h = h; it->text = (const char*)text; it->i1c = i1c;
    it->data = (void*)data; it->style = style; it->lo = lo; it->hi = hi; it->sel = sel;
    return it + 1;
}
static void build_world() {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    UI_G32(S_SCREEN_W) = 640;
    UI_G32(S_SCREEN_H) = 480;
    // the game's static initialisers (the Xlators, the colours) and UIBegin (the styles, the stamps, the windows)
    for (int i = 0; i < g_nfns; i++)
        if (g_fns[i].name[0] == '$') call_orig(g_fns[i].v10, 0, {});
    call_orig(0x004779a0, 0, {});
    W.screen = (gxCanvas*)wv(sizeof(gxCanvas));
    W.screen->format = 4; W.screen->pixels = (uint8_t*)0x7b000000; W.screen->w = 640; W.screen->h = 480; W.screen->pitch = 1280;
    W.screen->cx1 = 640; W.screen->cy1 = 480;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    W.group = (uint32_t*)wv(64);
    W.bytes = (uint8_t*)wv(64);
    W.ints = (int32_t*)wv(64 * 4);
    W.floats = (float*)wv(64 * 4);
    W.controls = (uint32_t*)wv(64);
    for (int i = 0; i < 3; i++) { W.buf[i] = (char*)wv(0x100); W.axis[i] = (UIScrollAxis*)wv(sizeof(UIScrollAxis)); }
    for (int i = 0; i < 12; i++) W.texts[i] = (char*)wv(0x100);
    W.scratch = (char*)wv(0x1400);
    W.scratch2 = (char*)wv(0x1400);
    W.name_buf = (char*)wv(0x130);
    W.dlg = (UIDialog*)wv(sizeof(UIDialog));
    W.menu = (UIMenu*)wv(sizeof(UIMenu));
    W.desc = (UIStyleDesc*)wv(sizeof(UIStyleDesc));
    W.cc = (UICustomControl*)wv(sizeof(UICustomControl));
    W.cc->vtbl = g_cc_vtbl;
    W.fstatic = (uint8_t*)wv(0x330);
    W.decal = (uint8_t*)wv(0x22c);
    W.decal_canvas = (gxCanvas*)wv(sizeof(gxCanvas));
    memcpy((void*)W.decal_canvas, (const void*)W.screen, sizeof(gxCanvas));
    W.decal_canvas->w = 40; W.decal_canvas->h = 30; W.decal_canvas->cx1 = 40; W.decal_canvas->cy1 = 30;
    // string lists (the game's constructor and AddEntry)
    const char* names[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet", "kilo"};
    for (int i = 0; i < 3; i++) {
        W.list[i] = (UIStringList*)wv(sizeof(UIStringList));
        call_orig(F_UIStringList_ctor, 1, {U(W.list[i]), 0, (uint32_t)(4 + 4 * i), (uint32_t)(12 + 8 * i)});
        for (int k = 0; k < 3 + 3 * i && k < 11; k++) call_orig(F_UIStringList_AddEntry, 1, {U(W.list[i]), 0, U(wstr(names[k]))});
    }
    // window 0: a 320 x 200 dialog with every item type
    UIDialogItem* it = W.items[0] = (UIDialogItem*)wv(sizeof(UIDialogItem) * 40);
    char* ms = (char*)wv(32);
    memcpy(ms, "one\0two\0three\0four\0", 20);
    W.buf_cap[0] = 32; W.buf_cap[1] = 20 * 3; W.buf_cap[2] = 12;
    it = it_(it, 1, 0, 0, 0, 0, 0, 0, 0, &W.group[0], 0);
    it = it_(it, 5, 0, 10, 8, 0, 0, wstr("A dialog\nwith two lines"), 0, 0, 0xe);
    it = it_(it, 2, 5, 20, 150, 0, 0, wstr("OK"), 0, 0, 2);
    it = it_(it, 2, -1, 90, 150, 0, 0, wstr("Cancel"), 0, (void*)&hb_callback, 2);
    it = it_(it, 2, -2, 160, 150, 0, 0, wstr("Go"), 0, 0, 3);
    it = it_(it, 2, 7, 230, 150, 0, 0, wstr("Repeat"), 1, (void*)&hb_callback, 4);
    it = it_(it, 3, 9, 10, 120, 0, 0, wstr("bbutton.stp"), 0, 0, 0);
    it = it_(it, 3, -1, 60, 120, 0, 0, wstr("bback.stp"), 1, (void*)&hb_callback, 0);
    it = it_(it, 4, 11, 'q', 0, 0, 0, 0, 0, (void*)&hb_callback, 0);
    it = it_(it, 6, 0, 200, 30, 0, 0, wstr("%.2f"), 0, &W.floats[0], 3, 2.0f, 1.5f);
    it = it_(it, 7, 0, 200, 45, 0, 0, wstr("%d"), 0, &W.ints[0], 3, 3.0f, 0.5f);
    it = it_(it, 8, 0, 200, 60, 0, 0, ms, 0, &W.ints[1], 5);
    it = it_(it, 9, 0, 20, 30, 100, 12, (const void*)1, 32, W.buf[0], 9);
    it = it_(it, 9, 0, 20, 50, 120, 36, (const void*)3, 20, W.buf[1], 9);
    it = it_(it, 10, 0, 20, 90, 80, 12, wstr("Press a key"), 0, &W.controls[0], 9);
    it = it_(it, 11, 0, 130, 30, 0, 0, wstr("Check"), 0, &W.bytes[0], 2);
    it = it_(it, 12, 0, 130, 45, 0, 0, wstr("Radio A"), 1, &W.ints[2], 2);
    it = it_(it, 12, 0, 130, 60, -1, 0, wstr("Radio group"), (int32_t)U(&W.group[0]), &W.ints[3], 2);
    it = it_(it, 13, 0, 130, 75, 0, 0, wstr("radio.stp"), 2, &W.ints[2], 0);
    it = it_(it, 14, 0, 250, 10, 0, 0, wstr("roll.stp"), 0, &W.ints[4], 0);
    it = it_(it, 15, 0, 40, 100, 100, 10, 0, 11, &W.floats[1], 0, 0.0f, 10.0f);
    it = it_(it, 16, 0, 160, 100, 0, 0, 0, 10, &W.floats[2], 0, -5.0f, 5.0f);
    it = it_(it, 16, 0, 175, 100, 0, 0, 0, 4, &W.floats[2], 1, -5.0f, 5.0f);
    it = it_(it, 17, 0, 290, 20, 10, 120, 0, 0, W.axis[0], 0);
    it = it_(it, 18, 0, 20, 175, 200, 10, 0, 0, W.axis[1], 0);
    it = it_(it, 19, 0, 300, 20, 0, 0, 0, 0, W.axis[0], 3);
    it = it_(it, 19, 0, 300, 40, 0, 0, 0, 0, W.axis[0], 2);
    it = it_(it, 20, 0, 150, 90, 100, 50, W.axis[0], 0, W.list[1], 0, 0, 0, &W.ints[5]);
    it = it_(it, 21, 0, 150, 140, 12, 6, 0, 0, W.list[2], 0, 0, 0, &W.ints[6]);
    it = it_(it, 22, 0, 5, 5, 300, 1, 0, (int32_t)0x12345678, 0, 0);
    it = it_(it, 23, 0, 240, 60, 40, 30, 0, 0, W.cc, 0);
    it = it_(it, 1, 1, 0, 0, 0, 0, 0, 0, &W.group[0], 0);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    W.nitems[0] = (int)(it - W.items[0]);
    call_orig(F_WidgetCreateWindow, 0, {U(wstr("dialog1.stp")), 160, 140, 320, 200, 1});
    W.win[0] = (WidgetWindow*)(uintptr_t)g_r_eax;
    call_orig(F_UIAddItems, 0, {U(W.win[0]), U(W.items[0])});
    {   // a title bar, by hand (as UIDoDialog makes one)
        call_orig(F_MemAlloc, 0, {0x234});
        void* t = (void*)(uintptr_t)g_r_eax;
        call_orig(F_Widget_ctor, 1, {U(t), 0, 4, 1, 320, 0x11});
        ((TitleBar*)t)->vtbl = (void*)(uintptr_t)VT_TitleBar;
        ((TitleBar*)t)->dragging = 0;
        call_orig(F_WidgetAddItem, 0, {U(W.win[0]), U(t)});
    }
    // window 1: full screen, 3D behind: menu buttons (their Tab keys), a list without an axis, a backwards slider, a
    // field with a negative width, a check box
    it = W.items[1] = (UIDialogItem*)wv(sizeof(UIDialogItem) * 12);
    it = it_(it, 5, 0, 30, 24, 0, 0, wstr("A menu"), 0, 0, 0x10);
    it = it_(it, 20, 0, 300, 100, 120, 90, 0, 0, W.list[0], 0, 0, 0, &W.ints[7]);
    it = it_(it, 15, 0, 300, 250, 120, 12, 0, -6, &W.floats[3], 0, -1.0f, 1.0f);
    it = it_(it, 9, 0, 300, 300, 80, 12, (const void*)1, -12, W.buf[2], 9);
    it = it_(it, 11, 0, 300, 330, 0, 0, wstr("Another"), 0, &W.bytes[1], 2);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    W.nitems[1] = (int)(it - W.items[1]);
    call_orig(F_WidgetCreateWindow, 0, {0, 0, 0, 640, 480, 2});
    W.win[1] = (WidgetWindow*)(uintptr_t)g_r_eax;
    call_orig(F_UIAddItems, 0, {U(W.win[1]), U(W.items[1])});
    for (int k = 0; k < 3; k++) {
        call_orig(F_MemAlloc, 0, {0x290});
        void* b = (void*)(uintptr_t)g_r_eax;
        call_orig(F_Button_ctor, 1, {U(b), 0, (uint32_t)(20 + k), 0x1ef, (uint32_t)(0x8e + 0x31 * k),
                                     U(wstr(k == 0 ? "Race" : k == 1 ? "Options" : "Quit")), k == 1 ? U(&hb_callback) : 0u, 2, 1, 0});
        ((Widget*)b)->vtbl = (void*)(uintptr_t)VT_MenuButton;
        call_orig(F_WidgetAddItem, 0, {U(W.win[1]), U(b)});
    }
    // window 2: empty
    call_orig(F_WidgetCreateWindow, 0, {U(wstr("empty.stp")), 100, 100, 50, 50, 0});
    W.win[2] = (WidgetWindow*)(uintptr_t)g_r_eax;
    call_orig(F_WidgetSetIdleFunction, 0, {U(W.win[0]), U(&hb_window_idle)});
    UI_GP(WidgetWindow, S_ACTIVE) = W.win[0];
    // the file boxes' statics: a list and a name of the world's; the detector's control
    UI_GP(void, S_FILE_LIST) = W.list[1];
    UI_GP(char, S_FILE_NAME) = W.name_buf;
    UI_GP(void, S_CD_CONTROL) = &W.controls[4];
    // a short item list for the dialog tests
    it = W.items[2] = (UIDialogItem*)wv(sizeof(UIDialogItem) * 8);
    it = it_(it, 5, 0, 10, 30, 0, 0, W.texts[0], 0, 0, 0xe);
    it = it_(it, 2, -2, 20, 150, 0, 0, wstr("Yes"), 0, 0, 2);
    it = it_(it, 2, -1, 120, 150, 0, 0, wstr("No"), 0, (void*)&hb_callback, 2);
    it = it_(it, 9, 0, 20, 60, 100, 12, (const void*)1, 16, W.buf[2], 9);
    it = it_(it, 23, 0, 240, 60, 40, 30, 0, 0, W.cc, 0);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    W.nitems[2] = (int)(it - W.items[2]);
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < W.win[k]->count; i++) W.widgets.push_back((Widget*)W.win[k]->widgets[i]);
    // FStaticText / Decal: never built by the game (no vtable); their fields by hand
    ((Widget*)W.fstatic)->x0 = 10; ((Widget*)W.fstatic)->y0 = 20;
    ((FStaticText*)W.fstatic)->fmt = wstr("static %% text");
    ((FStaticText*)W.fstatic)->font = fake_for("fstatic.fnt", 'FONT');
    ((Widget*)W.decal)->x0 = 30; ((Widget*)W.decal)->y0 = 40;
    ((Decal*)W.decal)->canvas = W.decal_canvas;
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
static const char k_chars[] = "abcdefghij klmnopqrstuvwxyz ABCDEFGH 0123456789 .,!?-";
static void random_text(char* s, int maxlen, int nl_pct) {
    const int n = irange(0, maxlen);
    for (int i = 0; i < n; i++) s[i] = chance(nl_pct) ? '\n' : k_chars[rnd() % (sizeof k_chars - 1)];
    s[n] = 0;
}
static Widget* pick_in(const WidgetWindow* w) {
    if (!w || w->count <= 0) return 0;
    return w->widgets[rnd() % (uint32_t)w->count];
}
static void randomize_world() {
    for (Widget* w : W.widgets) {
        w->over = chance(40); w->focus = chance(30); w->visible = chance(85); w->enabled = chance(85); w->dirty = chance(50);
        if (chance(10)) { w->x0 = w->x0 + irange(-8, 8); w->y0 = w->y0 + irange(-8, 8); }
        switch ((uint32_t)(uintptr_t)w->vtbl) {
        case VT_Button: case VT_MenuButton: ((Button*)w)->pressed = chance(40); break;
        case VT_BButton: ((BButton*)w)->pressed = chance(40); ((BButton*)w)->flash = chance(20); break;
        case VT_ArrowButton: case VT_ScrollButton: {
            ScrollButtonBase* s = (ScrollButtonBase*)w;
            s->pressed = chance(60);
            s->timer = chance(10) ? wildf() : range(-0.5f, 0.5f);
            s->rate = chance(10) ? wildf() : range(0.001f, 0.4f);
            if (chance(15) && (uint32_t)(uintptr_t)w->vtbl == VT_ArrowButton) ((ArrowButton*)w)->steps = irange(-2, 3);
            break;
        }
        case VT_ListBox: case VT_DropList: ((ListBox*)w)->scroll = irange(-1, 1); ((ListBox*)w)->dragging = chance(50); break;
        case VT_Slider: ((Slider*)w)->dragging = chance(60); break;
        case VT_ScrollBar: ((ScrollBar*)w)->dragging = chance(60); ((ScrollBar*)w)->grab_x = irange(-50, 50); ((ScrollBar*)w)->grab_y = irange(-50, 50); break;
        case VT_TitleBar: ((TitleBar*)w)->dragging = chance(60); ((TitleBar*)w)->dx = irange(-100, 0); ((TitleBar*)w)->dy = irange(-20, 0); break;
        case VT_Input: case VT_CDetect: {
            Input* in = (Input*)w;
            const int len = in->buf ? (int)strlen(in->buf) : 0;
            in->cursor = chance(5) ? irange(-2, len + 4) : irange(0, len);
            if (chance(10)) for (int k = 0; k < 8; k++) in->line_start[k] = irange(0, len);
            break;
        }
        }
    }
    for (int i = 0; i < 3; i++) {
        WidgetWindow* w = W.win[i];
        w->over = chance(50) ? pick_in(w) : 0;
        w->captured = chance(30) ? pick_in(w) : 0;
        w->focus = chance(60) ? pick_in(w) : 0;
        w->hidden = chance(30) ? rnd() & 3 : 0;
        w->disabled = chance(30) ? rnd() & 3 : 0;
        w->full_redraw = chance(40);
        w->next_group = chance(10) ? 0 : 1u << irange(0, 4);
        w->group = rnd() & 3;
        if (chance(20)) { w->x = w->x + irange(-20, 20); w->y = w->y + irange(-20, 20); }
    }
    for (int i = 0; i < 16; i++) W.bytes[i] = chance(10) ? (uint8_t)rnd() : (uint8_t)(rnd() & 1);
    for (int i = 0; i < 16; i++) W.ints[i] = irange(-2, 6);
    W.ints[4] = irange(-1, 6);
    for (int i = 0; i < 16; i++) W.floats[i] = chance(10) ? wildf() : range(-6.0f, 12.0f);
    for (int i = 0; i < 16; i++) W.controls[i] = rnd();
    for (int i = 0; i < 3; i++) {
        W.axis[i]->total = irange(0, 40); W.axis[i]->visible = irange(0, 20); W.axis[i]->pos = irange(-3, 40);
        W.list[i]->changes = (int32_t)rnd();
    }
    for (int i = 0; i < 3; i++) random_text(W.buf[i], W.buf_cap[i] > 2 ? W.buf_cap[i] - 2 : 0, i == 1 ? 8 : 0);
    for (int i = 0; i < 12; i++) random_text(W.texts[i], i < 6 ? 40 : 200, i % 3 == 0 ? 10 : 0);
    random_text(W.name_buf, 20, 0);
    UI_GF(S_UI_DT) = chance(10) ? wildf() : range(0.0f, 0.25f);
    UI_GP(void, S_CURSOR) = chance(50) ? 0 : fake_for("cursor2.stp", 'STMP');
    UI_G32(S_MOUSE_X) = irange(-10, 650); UI_G32(S_MOUSE_Y) = irange(-10, 490);
    UI_G8(S_EXIT) = chance(10);
    UI_GU32(S_DIALOG_IDLE) = chance(50) ? U(&hb_idle) : 0;
    UI_G32(S_FILE_SEL) = irange(-1, 8); UI_G32(S_FILE_LAST) = irange(-1, 8);
    UI_GP(WidgetWindow, S_ACTIVE) = chance(80) ? W.win[0] : chance(50) ? W.win[1] : chance(50) ? W.win[2] : 0;
    // the stubs' world
    HS->time = (int32_t)rnd();
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->cb_mask = (int32_t)rnd();
    HS->cb_calls = HS->idle_calls = HS->detect_calls = 0;
    HS->idle_exit_after = irange(1, 30);
    HS->detect_after = irange(1, 8);
    HS->grab_fail_at = chance(20) ? irange(1, 6) : -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->nfiles = irange(0, 6);
    for (int i = 0; i < 8; i++) sprintf(HS->files[i], i & 1 ? "dir\\file%d.trk" : "track%d.trk", irange(0, 99));
    HS->file_open_mask = (int32_t)rnd();
    HS->script_n = HS->script_pos = 0;
}
// a random input script (for the functions that run a window)
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x123, 0x124, 0x125, 0x127, 0x12e, 0x126, 0x128, 0x16, 'q', 'a', 'Z', '5', 0x141};
    const int n = irange(0, maxops);
    for (int i = 0; i < n && i < 62; i++) {
        int32_t* op = HS->script[i];
        const uint32_t r = rnd() % 10;
        op[1] = op[2] = 0;
        if (r < 3) { op[0] = OP_MOVE_W; op[1] = irange(0, 40); op[2] = irange(0, 479); }
        else if (r < 4) { op[0] = OP_MOVE_XY; op[1] = irange(-10, 650); op[2] = irange(-10, 490); }
        else if (r < 5) op[0] = OP_DOWN;
        else if (r < 6) op[0] = OP_UP;
        else if (r < 7) op[0] = chance(50) ? OP_RDOWN : OP_RUP;
        else if (r < 9) { op[0] = OP_KEY; op[1] = keys[rnd() % (sizeof keys / sizeof keys[0])]; }
        else op[0] = OP_NOP;
        if (op[0] == OP_MOVE_W && chance(60)) {            // a click: a move, a press and a release
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
        {0x004f665c, 0x004f6698},             // the Xlators' keys
        {0x004f6b78, 0x004f7228},             // ui / _widget / widget / uistyle strings
        {0x004f7230, 0x004f7300},
        {0x004e4db8, 0x004e4dbc},
        {0x004efbf8, 0x004efbfc},             // the current canvas
        {0x005024b8, 0x005024bc},             // __adjust_fdiv
        {0x005228d4, 0x005228d8}, {0x005228f4, 0x005228f8},   // the screen's size
        {0x00578cd0, 0x00578cdc}, {0x00578d50, 0x00578d5c}, {0x00578d60, 0x00578d6c}, {0x00578e88, 0x00578e94},
        {0x00578e98, 0x00578ea4}, {0x00578eb0, 0x00578ebc}, {0x00578ec0, 0x00578ecc}, {0x00578ef8, 0x00578f04},
        {0x00579050, 0x0057905c},             // the Xlators
        {0x00578d08, 0x00578d40},             // the end item
        {0x00578d48, 0x00578d4c}, {0x00578e78, 0x00578e7c}, {0x00578f40, 0x00578f44}, {0x00578e80, 0x00578e84},
        {0x00579730, 0x00579778},             // the running window, the window table
    };
    for (auto& k : keep) memcpy(DATA + (k[0] - 0x004e1000), keepfrom.data + (k[0] - 0x004e1000), k[1] - k[0]);
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static Widget* pick_vt(std::initializer_list<uint32_t> vts) {
    Widget* c[256];
    int n = 0;
    for (Widget* w : W.widgets)
        for (uint32_t v : vts)
            if ((uint32_t)(uintptr_t)w->vtbl == v && n < 256) c[n++] = w;
    return n ? c[rnd() % (uint32_t)n] : 0;
}
static Widget* pick_class(const char* cls) {
    struct { const char* c; std::initializer_list<uint32_t> v; } k[] = {
        {"StyleWidget", {VT_Button, VT_MenuButton, VT_CheckBox, VT_RadioButton, VT_StaticText, VT_Numeric, VT_IntNumeric, VT_Multi}},
        {"Button", {VT_Button, VT_MenuButton}}, {"MenuButton", {VT_MenuButton}}, {"BButton", {VT_BButton}},
        {"CheckBox", {VT_CheckBox}}, {"StampRoll", {VT_StampRoll}}, {"RadioButton", {VT_RadioButton}},
        {"BRadioButton", {VT_BRadioButton}}, {"ScrollButtonBase", {VT_ArrowButton, VT_ScrollButton}},
        {"ArrowButton", {VT_ArrowButton}}, {"ScrollButton", {VT_ScrollButton}}, {"StaticText", {VT_StaticText}},
        {"LineWidget", {VT_LineWidget}}, {"Numeric", {VT_Numeric}}, {"IntNumeric", {VT_IntNumeric}}, {"Multi", {VT_Multi}},
        {"ListBox", {VT_ListBox, VT_DropList}}, {"DropList", {VT_DropList}}, {"Input", {VT_Input, VT_CDetect}},
        {"CDetect", {VT_CDetect}}, {"Slider", {VT_Slider}}, {"ScrollBar", {VT_ScrollBar}}, {"HotKey", {VT_HotKey}},
        {"CustomWidget", {VT_CustomWidget}}, {"TitleBar", {VT_TitleBar}},
    };
    if (!strcmp(cls, "FStaticText")) return (Widget*)W.fstatic;
    if (!strcmp(cls, "Decal")) return (Widget*)W.decal;
    for (auto& e : k)
        if (!strcmp(cls, e.c)) return pick_vt(e.v);
    return W.widgets[rnd() % W.widgets.size()];                 // Widget
}
static uint16_t random_key() {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x123, 0x124, 0x125, 0x127, 0x12e, 0x126, 0x128, 0x16, 'q', 'a', 'Z', '5',
                                    0x141, 0, 0x1f, 0x7f, 0xff, 0x100};
    return chance(80) ? keys[rnd() % (sizeof keys / sizeof keys[0])] : (uint16_t)rnd();
}
static void widget_xy(const Widget* w, int32_t* x, int32_t* y) {        // window coordinates, usually on it
    if (w && chance(70)) {
        *x = irange(w->x0 - 6, w->x1 + 6);
        *y = irange(w->y0 - 6, w->y1 + 6);
    } else {
        *x = irange(-60, 700);
        *y = irange(-60, 500);
    }
}
static const char* text() { return W.texts[rnd() % 12]; }
static const char* short_text() { return W.texts[rnd() % 6]; }
static int32_t style() { return chance(90) ? irange(0, 23) : irange(0, 31); }
static uint32_t id_() { static const int32_t ids[] = {-2, -1, 0, 5, 7, 9, 11, 20, 21, 22, -66666}; return (uint32_t)ids[rnd() % 11]; }
static uint32_t var_ptr() { return chance(50) ? U(&W.ints[irange(0, 15)]) : U(&W.floats[irange(0, 15)]); }
static uint32_t name_ptr() {
    static const char* n[] = {"dialog1.stp", "fdialog.stp", "bbutton.stp", "slider.stp", "roll.stp", "x.stp"};
    return U(n[rnd() % 6]);
}
static void fill_random(void* p, uint32_t n) { uint8_t* b = (uint8_t*)p; for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)rnd(); }

// the widget a method runs on, pushed (most rounds) towards the states its branches need: focused, visible and
// enabled, pressed and over, dragging, a caret at the end of the text, a timer about to run out, values on a step
static void bias_self(Widget* w, const char* meth) {
    if (chance(25)) return;
    w->visible = chance(90); w->enabled = chance(90);
    if (!strcmp(meth, "CharHit")) w->focus = chance(80);
    if (strstr(meth, "Draw")) { w->focus = chance(50); w->over = chance(50); }
    switch ((uint32_t)(uintptr_t)w->vtbl) {
    case VT_Button: case VT_MenuButton: ((Button*)w)->pressed = chance(70); w->over = chance(70); break;
    case VT_BButton: ((BButton*)w)->pressed = chance(70); w->over = chance(70); break;
    case VT_ArrowButton: case VT_ScrollButton: {
        ScrollButtonBase* s = (ScrollButtonBase*)w;
        s->pressed = chance(85); w->over = chance(85);
        s->timer = chance(70) ? range(-0.05f, 0.1f) : range(-2.0f, 0.0f);
        s->rate = chance(70) ? range(0.0005f, 0.05f) : range(0.05f, 0.4f);
        if ((uint32_t)(uintptr_t)w->vtbl == VT_ArrowButton && chance(50)) {
            ArrowButton* a = (ArrowButton*)w;
            const float step = (a->hi - a->lo) / (float)(a->steps - 1);
            if (a->value && a->steps > 1) *a->value = a->lo + step * (float)irange(0, a->steps - 1) + (chance(50) ? range(-1e-5f, 1e-5f) : step * 0.5f);
        }
        break;
    }
    case VT_Slider: {
        Slider* s = (Slider*)w;
        s->dragging = chance(85);
        break;
    }
    case VT_ListBox: case VT_DropList: ((ListBox*)w)->dragging = chance(80); break;
    case VT_ScrollBar: ((ScrollBar*)w)->dragging = chance(80); break;
    case VT_TitleBar: ((TitleBar*)w)->dragging = chance(80); break;
    case VT_Input: case VT_CDetect: {
        Input* in = (Input*)w;
        const int len = in->buf ? (int)strlen(in->buf) : 0;
        const int r = irange(0, 3);
        in->cursor = r == 0 ? len : r == 1 ? 0 : irange(0, len);
        break;
    }
    }
}
// the arguments for f (words: ecx, edx first for a __thiscall); false: skip this round
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const char* nm = f.name;
    if (nm[0] == '$') return true;
    char cls[64] = "";
    const char* meth = nm;
    if (const char* sep = strstr(nm, "::")) {
        const size_t n = (size_t)(sep - nm) < 63 ? (size_t)(sep - nm) : 63;
        memcpy(cls, nm, n);
        cls[n] = 0;
        meth = sep + 2;
    }
    uint32_t* a = f.fast ? w + 2 : w;
    int32_t x, y;
    fill_random(W.scratch, 0x1400);
    // ---- WidgetWindow ----
    if (!strcmp(cls, "WidgetWindow")) {
        WidgetWindow* win = W.win[chance(45) ? 0 : chance(70) ? 1 : 2];
        w[0] = U(win);
        Widget* tw = pick_in(win);
        if (!strcmp(meth, "WidgetWindow")) {
            w[0] = U(W.scratch);
            a[0] = chance(70) ? name_ptr() : 0; a[1] = irange(-10, 400); a[2] = irange(-10, 300); a[3] = irange(1, 400); a[4] = irange(1, 300); a[5] = irange(0, 2);
        } else if (strstr(meth, "Mouse")) {
            widget_xy(tw, &x, &y);
            a[0] = (uint32_t)(x + win->x); a[1] = (uint32_t)(y + win->y);
        } else if (!strcmp(meth, "find_widget")) {
            widget_xy(tw, &x, &y);
            a[0] = (uint32_t)x; a[1] = (uint32_t)y;
        } else if (!strcmp(meth, "CharHit")) a[0] = random_key();
        else if (!strcmp(meth, "set_focus") || !strcmp(meth, "find_widget_index")) a[0] = chance(20) ? 0 : chance(80) ? U(tw) : U(pick_class("Widget"));
        else if (!strcmp(meth, "Draw")) a[0] = U(W.screen);
        else if (strstr(meth, "EnterGroup") || strstr(meth, "LeaveGroup")) a[0] = U(&W.group[irange(1, 7)]);
        else if (strstr(meth, "Group")) a[0] = chance(70) ? 1u << irange(0, 3) : rnd();
        else if (!strcmp(meth, "AddWidget")) a[0] = U(pick_class("Widget"));
        else if (!strcmp(meth, "SetDefault")) a[0] = id_();
        return true;
    }
    if (!strcmp(cls, "UIStringList")) {
        w[0] = U(W.list[rnd() % 3]);
        if (!strcmp(meth, "UIStringList")) { w[0] = U(W.scratch); a[0] = (uint32_t)irange(-1, 10); a[1] = (uint32_t)irange(1, 40); }
        else if (!strcmp(meth, "AddEntry")) a[0] = U(text());
        else if (!strcmp(meth, "GetEntry") || !strcmp(meth, "DeleteEntry")) a[0] = (uint32_t)irange(-2, 12);
        return true;
    }
    if (!strcmp(cls, "UICustomControl")) {
        W.cc->widget = (CustomWidget*)pick_vt({VT_CustomWidget});
        w[0] = U(W.cc);
        if (!strcmp(meth, "AddNotification")) { a[0] = rnd() % 4; a[1] = var_ptr(); a[2] = irange(1, 12); }
        else if (!strcmp(meth, "RemoveNotification")) {
            const Widget* g = W.cc->widget;
            a[0] = g && g->nnotes > 0 && chance(80) ? U(g->notes[rnd() % (uint32_t)g->nnotes].ptr) : var_ptr();
        } else if (!strcmp(meth, "AddItems")) a[0] = U(W.items[2]);
        return true;
    }
    if (cls[0] && strcmp(cls, "Widget") && !strchr(cls, '(')) {
        // ---- constructors: a fresh (random) block ----
        if (!strcmp(meth, cls)) {
            w[0] = U(W.scratch);
            const int32_t s = style();
            int32_t* v = &W.ints[irange(0, 15)];
            float* fv = &W.floats[irange(0, 15)];
            if (!strcmp(cls, "StyleWidget")) { a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = s; }
            else if (!strcmp(cls, "Button")) {
                a[0] = id_(); a[1] = irange(0, 300); a[2] = irange(0, 200); a[3] = U(short_text()); a[4] = chance(50) ? U(&hb_callback) : 0;
                a[5] = s; a[6] = rnd() & 1; a[7] = rnd() & 1;
            } else if (!strcmp(cls, "BButton")) {
                a[0] = id_(); a[1] = irange(0, 300); a[2] = irange(0, 200); a[3] = name_ptr(); a[4] = chance(50) ? U(&hb_callback) : 0;
                a[5] = rnd() & 1; a[6] = rnd() & 1;
            } else if (!strcmp(cls, "CheckBox")) { a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = U(text()); a[3] = U(&W.bytes[irange(0, 15)]); a[4] = s; }
            else if (!strcmp(cls, "StampRoll")) { a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = name_ptr(); a[3] = chance(70) ? U(v) : 0; }
            else if (!strcmp(cls, "RadioButton")) {
                const bool mode = chance(40);
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = U(text()); a[3] = mode ? U(&W.group[irange(0, 3)]) : (uint32_t)irange(0, 4);
                a[4] = chance(80) ? U(v) : 0; a[5] = s; a[6] = mode;
            } else if (!strcmp(cls, "BRadioButton")) { a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = name_ptr(); a[3] = irange(0, 4); a[4] = U(v); }
            else if (!strcmp(cls, "ScrollButtonBase")) { a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = U(fake_for("sbb.stp", 'STMP')); }
            else if (!strcmp(cls, "ArrowButton")) {
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = U(fv); a[3] = fbits(range(-10, 0)); a[4] = fbits(range(0, 10));
                a[5] = irange(-3, 12); a[6] = chance(50) ? 1 : (uint32_t)-1; a[7] = U(fake_for("arrow.stp", 'STMP'));
            } else if (!strcmp(cls, "ScrollButton")) {
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = U(W.axis[rnd() % 3]); a[3] = irange(-1, 2); a[4] = U(fake_for("sb.stp", 'STMP'));
            } else if (!strcmp(cls, "StaticText")) { a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = U(text()); a[3] = s; }
            else if (!strcmp(cls, "LineWidget")) { a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = irange(-3, 100); a[3] = irange(-3, 100); a[4] = rnd(); }
            else if (!strcmp(cls, "Numeric")) {
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = U(fv); a[3] = fbits(range(-5, 5)); a[4] = fbits(range(-5, 5));
                a[5] = U(chance(50) ? "%.2f" : "%6.1f"); a[6] = s;
            } else if (!strcmp(cls, "IntNumeric")) {
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = U(v); a[3] = fbits(range(-5, 5)); a[4] = fbits(range(-5, 5));
                a[5] = U(chance(50) ? "%d" : "%4d mph"); a[6] = s;
            } else if (!strcmp(cls, "Multi")) {
                char* ms = W.scratch + 0xc00;
                int o = 0;
                for (int k = irange(0, 6); k > 0; k--) o += sprintf(ms + o, "opt%d", irange(0, 99)) + 1;
                ms[o] = 0;
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = U(v); a[3] = U(ms); a[4] = s;
            } else if (!strcmp(cls, "ListBox")) {
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = irange(8, 200); a[3] = irange(8, 150); a[4] = U(W.list[rnd() % 3]); a[5] = U(v);
                a[6] = chance(60) ? U(W.axis[rnd() % 3]) : 0;
            } else if (!strcmp(cls, "DropList")) {
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = irange(1, 12); a[3] = irange(1, 10); a[4] = U(W.list[rnd() % 3]); a[5] = U(v);
            } else if (!strcmp(cls, "Input")) {
                const int width = irange(-20, 32), lines = irange(1, 3);
                char* b = W.scratch + 0xc00;
                random_text(b, (width < 0 ? -width : width) * lines > 2 ? (width < 0 ? -width : width) * lines - 2 : 0, 5);
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = irange(8, 200); a[3] = irange(8, 60); a[4] = U(b); a[5] = width;
                a[6] = lines; a[7] = s; a[8] = rnd() & 1;
            } else if (!strcmp(cls, "CDetect")) {
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = irange(8, 200); a[3] = irange(8, 60); a[4] = U(text());
                a[5] = U(&W.controls[irange(0, 12)]); a[6] = s;
            } else if (!strcmp(cls, "Slider")) {
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = irange(8, 200); a[3] = irange(4, 30); a[4] = U(fv);
                a[5] = fbits(range(-10, 0)); a[6] = fbits(range(0, 10)); a[7] = chance(50) ? irange(2, 20) : -irange(2, 20);
            } else if (!strcmp(cls, "ScrollBar")) {
                a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = irange(8, 200); a[3] = irange(8, 150); a[4] = U(W.axis[rnd() % 3]); a[5] = irange(0, 1);
            } else if (!strcmp(cls, "Widget")) { a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = irange(0, 300); a[3] = irange(0, 200); }
            return true;
        }
        Widget* self = pick_class(cls);
        if (!self) return false;
        w[0] = U(self);
    } else if (!strcmp(cls, "Widget")) {
        if (!strcmp(meth, "Widget")) {                       // Widget::Widget
            w[0] = U(W.scratch);
            a[0] = irange(0, 300); a[1] = irange(0, 200); a[2] = irange(0, 300); a[3] = irange(0, 200);
            return true;
        }
        w[0] = U(pick_class("Widget"));
    }
    if (cls[0] && !strchr(cls, '(')) {
        // ---- a widget's method ----
        Widget* self = (Widget*)(uintptr_t)w[0];
        if (self != (Widget*)W.fstatic && self != (Widget*)W.decal) bias_self(self, meth);
        if (strstr(meth, "deleting destructor")) a[0] = rnd() & 3;
        else if (strstr(meth, "Mouse") || !strcmp(meth, "HitTest") || !strcmp(meth, "is_hot_area")) {
            widget_xy(self, &x, &y);
            if ((uint32_t)(uintptr_t)self->vtbl == VT_Slider && chance(60)) {        // along the track
                const Slider* s = (const Slider*)self;
                x = s->tx + irange(-3, s->tw + 3); y = s->ty + irange(-3, s->th + 3);
            }
            a[0] = (uint32_t)x; a[1] = (uint32_t)y;
        } else if (!strcmp(meth, "Move") || !strcmp(meth, "Resize")) { a[0] = irange(-50, 400); a[1] = irange(-50, 300); }
        else if (!strcmp(meth, "CharHit")) {
            a[0] = random_key();
            const uint32_t vt = (uint32_t)(uintptr_t)self->vtbl;
            if ((vt == VT_Input || vt == VT_CDetect) && chance(60)) {      // the field's own keys
                static const uint16_t ek[] = {0x123, 0x124, 0x125, 0x127, 0x12e, 8, 'a', ' ', '~', 0x16};
                a[0] = ek[rnd() % 10];
            }
        } else if (!strcmp(meth, "Draw")) {
            const WidgetWindow* win = self->window;
            a[0] = win && chance(70) ? U(&win->canvas) : U(W.screen);
        } else if (!strcmp(meth, "Callback")) { a[0] = rnd() % 4; a[1] = chance(50) ? 0 : var_ptr(); }
        else if (!strcmp(meth, "AddNotification")) { a[0] = rnd() % 4; a[1] = var_ptr(); a[2] = irange(1, 12); }
        else if (!strcmp(meth, "RemoveNotification"))
            a[0] = self->nnotes > 0 && self->nnotes <= 32 && chance(80) ? U(self->notes[rnd() % (uint32_t)self->nnotes].ptr) : var_ptr();
        else if (!strcmp(meth, "GetLine")) { a[0] = (uint32_t)irange(-1, 4); a[1] = U(W.scratch2); }
        return true;
    }
    // ---- plain functions ----
    const int32_t s = style();
#define IS(n) (!strcmp(nm, n))
    if (IS("UIAddStyle") || IS("UIAddDynamicStyle")) {
        const uint32_t d = chance(60) ? 0x004f6698 + 0x34 * (rnd() % 24) : U(W.desc);
        fill_random(W.desc, sizeof(UIStyleDesc));
        W.desc->font = chance(80) ? "desc.fnt" : 0; W.desc->stamp = chance(50) ? "desc.stp" : 0;
        if (IS("UIAddStyle")) { a[0] = (uint32_t)irange(0, 31); a[1] = d; }
        else a[0] = d;
    } else if (IS("UIRemoveStyle") || IS("get_style")) a[0] = (uint32_t)(chance(95) ? irange(0, 31) : irange(-4, 40));
    else if (strstr(nm, "ASSERT_MSG")) { a[0] = rnd() & 1; a[1] = U("assert %d"); }
    else if (IS("UIStyleGetBounds")) { a[0] = s; a[1] = chance(85) ? U(text()) : 0; a[2] = irange(-50, 400); a[3] = irange(-50, 300);
                                       a[4] = U(W.scratch2); a[5] = U(W.scratch2 + 4); a[6] = U(W.scratch2 + 8); a[7] = U(W.scratch2 + 12); }
    else if (IS("charcount")) { a[0] = U(text()); a[1] = chance(60) ? '\n' : chance(50) ? ' ' : (uint32_t)(rnd() & 0xff); }
    else if (IS("UIStylePointToIndex")) { a[0] = s; a[1] = U(short_text()); a[2] = irange(-20, 300); a[3] = irange(-20, 100); }
    else if (IS("UIStyleIndexToPoint")) { a[0] = s; a[1] = U(text()); a[2] = irange(-2, 60); a[3] = U(W.scratch2); a[4] = U(W.scratch2 + 4); }
    else if (IS("UIStyleWidth") || IS("UIStyleHeight")) { a[0] = s; a[1] = chance(85) ? U(text()) : 0; }
    else if (IS("UIStyleDraw")) { a[0] = s; a[1] = irange(-50, 640); a[2] = irange(-50, 480); a[3] = chance(90) ? U(text()) : 0; a[4] = chance(90) ? irange(0, 3) : irange(4, 7); }
    else if (IS("UIStyleWordWrap")) { a[0] = s; a[1] = chance(80) ? irange(20, 300) : irange(-5, 20); a[2] = U(W.scratch2); a[3] = U(text()); }
    else if (IS("get_width")) { a[0] = s; a[1] = U(W.texts[0]); a[2] = U(W.texts[0] + irange(-3, 40)); }
    else if (IS("WidgetCreateWindow")) { a[0] = chance(70) ? name_ptr() : 0; a[1] = irange(-10, 400); a[2] = irange(-10, 300); a[3] = irange(1, 400); a[4] = irange(1, 300); a[5] = irange(0, 2); }
    else if (IS("WidgetDestroyWindow")) a[0] = chance(90) ? U(W.win[rnd() % 3]) : chance(50) ? 0 : U(W.scratch);
    else if (IS("WidgetExit")) a[0] = id_();
    else if (IS("WidgetEnterGroup") || IS("WidgetLeaveGroup")) { a[0] = U(W.win[rnd() % 3]); a[1] = U(&W.group[irange(1, 7)]); }
    else if (strstr(nm, "Widget") == nm && strstr(nm, "Group")) { a[0] = U(W.win[rnd() % 3]); a[1] = chance(70) ? 1u << irange(0, 3) : rnd(); }
    else if (IS("WidgetAddItem")) { a[0] = U(W.win[rnd() % 3]); a[1] = U(pick_class("Widget")); }
    else if (IS("WidgetSetIdleFunction")) { a[0] = U(W.win[rnd() % 3]); a[1] = chance(50) ? U(&hb_window_idle) : 0; }
    else if (IS("WidgetExecuteWindow")) a[0] = U(W.win[rnd() % 3]);
    else if (IS("WidgetSetDefault")) { a[0] = U(W.win[rnd() % 3]); a[1] = id_(); }
    else if (IS("UIDoMenu")) {
        struct Item { const char* t; int32_t id; }* items = (Item*)(W.scratch2);
        const int n = irange(0, 5);
        for (int k = 0; k < n; k++) { items[k].t = short_text(); items[k].id = irange(0, 9); }
        items[n].t = 0;
        W.menu->title = text(); W.menu->background = chance(60) ? "menu.stp" : 0; W.menu->back = rnd() & 1; W.menu->default_id = irange(-2, 9);
        W.menu->items = (decltype(W.menu->items))items;
        a[0] = U(W.menu);
    } else if (IS("_UIAddItems")) { a[0] = U(W.win[rnd() % 3]); a[1] = U(W.items[rnd() % 3]); }
    else if (IS("UIDoDialog")) {
        W.dlg->title = text(); W.dlg->background = chance(60) ? "dialog1.stp" : 0; W.dlg->default_id = id_();
        W.dlg->items = W.items[chance(30) ? 0 : 2]; W.dlg->idle = chance(50) ? &hb_idle : 0;
        a[0] = U(W.dlg);
        const bool full = chance(30);
        a[1] = full ? 640 : 320; a[2] = full ? 480 : 200;
        a[3] = chance(70) ? (uint32_t)-999 : irange(0, 300); a[4] = chance(70) ? (uint32_t)-999 : irange(0, 280); a[5] = irange(0, 2);
    } else if (strstr(nm, "Group")) a[0] = chance(70) ? 1u << irange(0, 3) : rnd();
    else if (IS("UIDoDratBox") || strstr(nm, "UIDoOkCancelBox(char const *,char const *,") || IS("UIDoYesNoBox")) { a[0] = U(short_text()); a[1] = U(text()); a[2] = chance(50) ? U(&hb_idle) : 0; }
    else if (strstr(nm, "UIDo") == nm && strstr(nm, "Box") && !strstr(nm, "Input") && !strstr(nm, "File")) { a[0] = U(short_text()); a[1] = U(text()); }
    else if (IS("UIDoInputBox")) {
        char* b = W.scratch2;
        const int max = irange(1, 60);
        random_text(b, max > 1 ? max - 1 : 0, 0);
        a[0] = U(short_text()); a[1] = U(text()); a[2] = U(b); a[3] = max;
    } else if (IS("UIDoOpenFileBox") || IS("UIDoSaveFileBox")) {
        char* out = W.scratch2;
        sprintf(out, chance(70) ? "name%d.trk" : "noext%d", irange(0, 99));
        a[0] = U(short_text()); a[1] = U(text()); a[2] = U(chance(70) ? "*.trk" : "*"); a[3] = U(out); a[4] = irange(1, 60);
        a[5] = U(chance(70) ? "tracks\\" : "");
    } else if (IS("inputbox_paste")) a[0] = rnd();
    else if (IS("file_idle") || IS("cdetect_idle")) { a[0] = U(W.scratch2); }
    else if (IS("file_exists")) a[0] = U(text());
    else if (IS("CreateMultiString")) {
        a[0] = U(W.scratch2);
        a[1] = U(short_text());
        const int n = irange(0, 8);
        for (int k = 0; k < n; k++) a[2 + k] = U(short_text());
        a[2 + n] = 0;
    }
#undef IS
    return true;
}

// ---- main -----------------------------------------------------------------------------------------------------------------------
static bool is_modal(const char* nm) {
    return strstr(nm, "UIDo") == nm || !strcmp(nm, "WidgetExecuteWindow") || !strcmp(nm, "WidgetUpdate") ||
           !strcmp(nm, "CDetect::MouseUp") || strstr(nm, "WidgetWindow::Mouse") || strstr(nm, "WidgetWindow::CharHit");
}
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    const int rounds = argc > 1 ? atoi(argv[1]) : 24;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    const char* only = getenv("VP_ONLY");
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_ui.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    GetCurrentThreadStackLimits(&g_stack_lo, &g_stack_hi);
    {
        HMODULE m = GetModuleHandleA(0);
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
        g_self_lo = (uintptr_t)m; g_self_hi = (uintptr_t)m + nt->OptionalHeader.SizeOfImage;
    }
    // the stubs
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
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    build_world();
    mem_save(g_pristine);
    printf("world: %d widgets in 3 windows, %u KB of heap used\n", (int)W.widgets.size(), (HS->heap_next - A_HEAP) / 1024);

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
    long long both_fault = 0;
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0;
        const int nr = is_modal(f.name) || f.name[0] == '$' ? rounds : rounds * 10;
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            randomize_world();
            if (poisoned) poison(g_pristine);
            random_script(is_modal(f.name) ? 14 : 4);
            uint32_t words[72];
            if (!make_args(f, words)) { skipped++; continue; }
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            mem_save(g_snap);
            const Result ro = run(f, false, words);
            const bool changed = mem_diff(g_snap) != 0;
            fn_changed |= changed;
            // the footprint: a pure function changes nothing; a listed one covers every change (the original's pass; the
            // stubs' state block excepted)
            if (!fp.replay_only && !ro.fault) {
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
        if (trace) printf("%08x %-64s %s%s (%d checks, %d faulted)\n", f.v10, f.name, fn_bad ? "BAD" : "ok", fn_changed ? "" : " (never changed memory)", fn_checks, fn_faults);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    printf("%d functions (%d pure, %d replay_only, %d others), %d listed twice; %lld checks (%lld on poisoned .data), %lld logged words "
           "compared: %d differ, %d footprint violations, %d checks faulted (%lld in both, the same way, unless counted above), %d skipped; "
           "%d functions bad\n", g_nfns, pure_n, replay_n, g_nfns - pure_n - replay_n, dup, checks, poisoned_checks, log_words, differ,
           fp_bad, faults, both_fault, skipped, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    return differ || fp_bad || dup ? 1 : 0;
}
