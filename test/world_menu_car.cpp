// world_menu_car.cpp -- M3 UI stage, step U2 (group A): every rewrite of mcar.obj (hook/menu_car.cpp, the garage setup
// editor) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_menu_car.cpp
//        /Fo<dir>\ /Fe<dir>\world_menu_car.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_menu_car.exe [rounds] [seed]          (VP_TRACE=1: one line per function; VP_ONLY=text: only those)
//   the fix build: the same with /DVP_MENU_FIXES (below)
//
// Loads out\race_v10.exe at 0x400000 the way test/world_ui.cpp does (whose loader, memory comparison, input script,
// 2D stubs and raw call this copies) and includes hook/menu_car.cpp, with PORT_FN redefined to list each function: its
// v1.0 address, the rewrite, its calling convention, stack arguments (in dwords) and return (width, or a float in st0),
// and its footprint.
//
// The world. Once, at start: the game's own static initialisers of the UI library and mcar.obj (their Xlators, the
// colours), UIBegin (the style table: fonts, palettes and stamps come from the stubs), a full-screen window made by the
// game's WidgetCreateWindow, and the seven controls -- BalanceControl, ChassisControl, AlignControl, DrivetrainControl
// (with its TorqueCurveControl), AeroControl, FileControl -- built in the arena with their v1.0 vtables and stamps, added
// by the game's _UIAddItems (so their original Added builds their pages' widgets: numbers, arrows, radio buttons,
// groups) and activated by the game's WidgetWindow::Activate (their original Create adds their notifications). The
// window is the running one. A fake Tire, the graph callbacks' arguments, a spare setup and setup set, the locale
// (0x509354), a user directory. This is the pristine state.
//
// Then for EVERY listed function, `rounds` times (ten times that for a function that doesn't run a dialog): pristine
// restored; half the rounds POISONED (every byte of .data/.bss random, but for what's read as a string or followed as a
// pointer -- the UI library's data, mcar.obj's strings and Xlators, the running window and window table, the current
// canvas, graph.obj's formats, the locale, the screen size, __adjust_fdiv -- and the setup, car file, CarData and set, which get the round's
// random values: a garage full of NaNs would overrun the original's sprintf buffers, the FIX CANDIDATEs in
// hook/menu_car.cpp); the round's state random: the setup's settings (fractions, sometimes NaN / infinite / huge / 0),
// its name (sometimes '*'-marked or empty) and its loaded copy (the same or not), the car file's limits (pairs sometimes
// equal, so the greyed-out branches run), gear count and ratios (the first sometimes 0), the CarData, the colours (two
// sometimes equal), the set (versions and sizes sometimes wrong, names, slot), the controls' pages (in their tables'
// range), the function-local statics' guard bits (mostly built, sometimes not), the Tire's and the callbacks'
// arguments; the stubs' answers (files there or not, of the right size or not, loads and saves failing). For a function
// that runs a dialog (MenuEditCar, file_cb, the save / load dialogs, the File buttons' callbacks) a random input script:
// mouse moves onto the running window's widgets, presses and releases, keys (Escape, Return, space, Tab, arrows,
// letters...), idle frames -- ended, if the dialog doesn't end itself, by a watchdog after 40 frames (which ends nested
// dialogs too). Snapshot; the stack below filled with one pattern (after the FPU is set up, and 512 bytes deeper than
// the call's own frame begins, so every local a function leaves uninitialised -- the save dialog's name field reads 16
// bytes past its buffer -- reads the pattern in both passes); the ORIGINAL runs from a raw call thunk (ecx / edx /
// the stack set exactly; eax / edx / st0, the x87 stack depth, the bytes popped, ebx / esi / edi / ebp recorded); its
// results kept, the snapshot restored, the REWRITE runs the same way. Compared: every byte of .data/.bss/.idata and the
// 1 MB arena (a dword where both passes left a pointer into this thread's stack counts as equal), the return value (a
// float's st0 bit for bit), the call logs of every stub, faults. Every byte the original changed must lie in the
// rewrite's footprint (unless it's replay_only; the stubs' state block excepted). The x87 runs at 24 or 53 bits.
// Two allowances, both counted and printed: a dword where the original left a return address into itself and the
// rewrite one into this program (UIDoYesNoCancelBox copies it into a widget from past its frame) counts as equal; and a
// call still running after 2 s (a watchdog thread makes it raise where it is) is a fault -- if both passes are stuck
// that way, their logs are compared as far as the shorter one goes.
//
// Stubbed (a jump to a logger; state in the arena): as world_ui (MemAlloc / delete, LogReport / LogPanic, the input,
// PTimeNow, the 2D calls that draw or measure, stamps, fonts, palettes, canvases, the screen, Xlator::xlate, sprintf,
// LocaleConvertNumeric, tolower, atexit, the CRT's fatal paths, FileOpen / FileClose), and the files and resources:
// Win32GetUserDirectory, FileSize / FileReadExact / FileCreate / FileWrite / FileCreateDirectory (a setup set image in
// the arena), ResourceSetMustLoad / ResourceSetUnload, CarFileLoadSetup / CarFileLoad (both) / CarFileSaveSetup /
// CarFileLoadDefaultSetup (a template setup), HTMLBegin / HTMLWrite / HTMLWriteLn / HTMLEnd, TireCreate / TireDestroy
// (the fake Tire). The game's own code runs everywhere else: the whole widget toolkit (UIDoDialog, _UIAddItems, the
// widgets, UIDoYesNoCancelBox, UIStringList, UIStyleDraw), the graphs (GraphDrawAxes, GraphDrawLinePlot, calling the
// original callbacks), CarFileCombine, Tire::GetForce, Damper, PowerCurve::Setup, UIDialogItem's and Xlator's
// constructors, strncpy, __ftol -- and mcar.obj's own functions wherever a rewrite calls them by address (each rewrite
// is checked against its original with the same callees).
//
// Built with /DVP_MENU_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Menus"). Every
// function is still compared as above, on states kept clear of the fixed cases: the car file's gear count never past 6
// (the random world's names -- the car's, the setup's up to 30 characters, at most 24 more typed -- never reach the
// others: a car name over 27 characters, a setup name of 62 or more, one with no end). Then directed_fix_tests: for
// each fix, the bad case run on the rewrite alone -- a clean return (no fault, the bytes popped, ebx / esi / edi / ebp
// kept), what the fix promises, and all memory and the call logs compared with the ORIGINAL run on the nearest case it
// gets right (the name cut where the fix cuts it, six gears, a short car name at the same address), with guard bytes
// after the setup checked untouched -- and, where running it can't take this program down, that the original really
// goes wrong there. (A name typed to the save dialog's limit is compared with the original outright: its frame holds
// the field's 64 bytes.) Without it (VP_FAITHFUL) every rewrite must match its original bit for bit.
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

#ifndef VP_MENU_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#define MENU_FIXES 0
#else
#define MENU_FIXES 1
#endif
#define UI_FIXES 0                  // (the toolkit here is the game's own code, in either build)
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

#include "../hook/menu_car.cpp"

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
// a return address into the function under test, as the stock dialogs leave one in a widget's copy of their frame (the
// original's: within its code; the rewrite's: within this program's): the two passes' differ, and count as equal
static uint32_t g_fn_lo, g_fn_hi;
static uintptr_t g_code_lo, g_code_hi;
static long long ret_pairs;
static bool ret_pair(uint32_t a, uint32_t b) { return a >= g_fn_lo && a < g_fn_hi && b >= g_code_lo && b < g_code_hi; }
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
            if (ret_pair(a, b)) { ok = true; i = s + 4; ret_pairs++; break; }
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
    // the garage's
    uint32_t cf_mask;                 // bit 0: CarFileLoadSetup, 1: CarFileLoad(file), 2: CarFileSaveSetup, 3: FileCreate,
                                      // 4: HTMLBegin succeed
    int32_t file_size;                // FileSize's answer
    int32_t def_calls;                // CarFileLoadDefaultSetup's count (its setups differ)
    uint8_t tmpl_setup[0xd4];         // what CarFileLoadSetup / LoadDefaultSetup load
    uint8_t set_img[0x6ac];           // what FileReadExact reads
    char user_dir[64];                // Win32GetUserDirectory's
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
static uint32_t g_text_len, g_fff_len;                   // the last gxText's and FileFindFirst's string lengths
static void __cdecl stub_gxText(int32_t x, int32_t y, const char* s, uint32_t col) {
    L('TEXT'); L((uint32_t)x); L((uint32_t)y); LS(s); L(col); L_canvas();
    g_text_len = readable(s, 1) ? (uint32_t)strnlen(s, 0x10000) : 0xffffffffu;
}
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
    L('SPRF'); L(UI_FIXES ? 'DEST' : P(buf)); LS(fmt);      // (the fix build: Numeric formats into its own buffer)
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
    g_fff_len = readable(pat, 1) ? (uint32_t)strnlen(pat, 0x10000) : 0xffffffffu;
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
        sub esp, 0x200                  // the callee's frame well inside what fill_stack filled
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
    for (uint32_t i = 0; i < sizeof buf / 4; i++) buf[i] = UI_FIXES ? 0u : 0xcdcdcdcdu;
}
static unsigned g_pc;
static unsigned g_unmask;                                 // exceptions unmasked for the call (the directed fix tests)
// a hang watchdog: a call still running after 2 s (a loop the poisoned world never lets end without a frame's input) is
// made to raise 0xe0000d06 where it is, which the call's handler takes as a fault (at the address it hung)
static HANDLE g_main_thread;
static volatile LONG g_wd_armed;
static volatile DWORD g_wd_deadline;
static volatile uint32_t g_hang_eip;
static void hang_trap() { RaiseException(0xe0000d06u, 0, 0, 0); }
static DWORD WINAPI watchdog_thread(void*) {
    for (;;) {
        Sleep(50);
        if (g_wd_armed && (LONG)(GetTickCount() - g_wd_deadline) > 0) {
            SuspendThread(g_main_thread);
            if (g_wd_armed) {
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_CONTROL;
                GetThreadContext(g_main_thread, &c);
                g_hang_eip = c.Eip;
                c.Eip = (DWORD)(uintptr_t)&hang_trap;
                SetThreadContext(g_main_thread, &c);
                g_wd_armed = 0;
            }
            ResumeThread(g_main_thread);
        }
    }
}
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
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    if (g_unmask) _controlfp_s(&cw, 0, g_unmask);
    fill_stack();                                         // (after the _controlfp_s calls, whose frames it would leave behind)
    __try {
        g_wd_deadline = GetTickCount() + 2000;
        g_wd_armed = 1;
        raw_call();
        g_wd_armed = 0;
    } __except (fault_filter(GetExceptionInformation())) {
        g_wd_armed = 0;
        r.fault = 1; r.code = g_fault_code; r.eip = g_fault_code == 0xe0000d06u ? g_hang_eip : g_fault_eip;
    }
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

// ---- the garage's stubs: files, resources, the car file, HTML, the tire ------------------------------------------------------------
static uint32_t hash_bytes(const void* p, uint32_t n) {
    if (!readable(p, n ? n : 1)) return 'BADP';
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static const char* __cdecl stub_UserDir() { L('UDIR'); return HS->user_dir; }
static void __cdecl stub_ResMust(const char* s) { L('RSML'); LS(s); }
static void __cdecl stub_ResUnload(const char* s) { L('RSUL'); LS(s); }
static uint8_t __cdecl stub_CarFileLoadSetup(uint8_t* setup, const char* car, const char* dir) {
    L('CFLS'); L(P(setup)); LS(car); LS(dir);
    if (!(HS->cf_mask & 1)) return 0;
    memcpy(setup, HS->tmpl_setup, 0xd4);
    return 1;
}
static uint8_t __cdecl stub_CarFileLoadFile(void* file, const char* name, uint8_t* flags) {
    L('CFLF'); L(P(file)); LS(name); L(P(flags));
    return (HS->cf_mask >> 1) & 1;
}
static uint8_t __cdecl stub_CarFileLoadData(void* data, const void* setup, const char* name, uint8_t* flags) {
    L('CFLD'); L(P(data)); L(P(setup)); LS(name); L(P(flags));
    return 1;
}
static uint8_t __cdecl stub_CarFileSaveSetup(const uint8_t* setup, const char* car, const char* dir) {
    L('CFSS'); L(P(setup)); L(hash_bytes(setup, 0xd4)); LS(car); LS(dir);
    return (HS->cf_mask >> 2) & 1;
}
static void __cdecl stub_CarFileLoadDefaultSetup(uint8_t* setup, const char* car, const char* dir) {
    L('CFLD'); L(P(setup)); LS(car); LS(dir);
    if (!readable(setup, 0xd4)) return;
    memcpy(setup, HS->tmpl_setup, 0xd4);
    setup[0x94] = (uint8_t)('A' + (HS->def_calls++ & 15));
    setup[0x95] = 0;
}
static int32_t __cdecl stub_FileSize(int32_t fd) { L('FSIZ'); L((uint32_t)fd); return HS->file_size; }
static uint8_t __cdecl stub_FileReadExact(int32_t fd, void* buf, int32_t n) {
    L('FRDX'); L((uint32_t)fd); L(P(buf)); L((uint32_t)n);
    if (n > 0 && n <= 0x6ac && readable(buf, (uint32_t)n)) memcpy(buf, HS->set_img, (size_t)n);
    return 1;
}
static int32_t __cdecl stub_FileCreate(const char* path) { L('FCRE'); LS(path); return (HS->cf_mask >> 3) & 1 ? 7 : 0; }
static uint8_t __cdecl stub_FileWrite(int32_t fd, const void* p, int32_t n) {
    L('FWRT'); L((uint32_t)fd); L((uint32_t)n); L(hash_bytes(p, n > 0 && n < 0x10000 ? (uint32_t)n : 0));
    return 1;
}
static uint8_t __cdecl stub_FileCreateDirectory(const char* path) { L('FMKD'); LS(path); return 1; }
static uint8_t __cdecl stub_HTMLBegin(const char* path, const char* title) { L('HBEG'); LS(path); LS(title); return (HS->cf_mask >> 4) & 1; }
static void __cdecl stub_HTMLWrite(const void* style, const char* fmt, ...) {
    L('HWRT'); L(P(style)); LS(fmt);
    va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap);
}
static void __cdecl stub_HTMLWriteLn(const char* fmt, ...) { L('HWLN'); LS(fmt); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); }
static void __cdecl stub_HTMLEnd() { L('HEND'); }
static uint8_t* g_tire;                                   // the fake Tire (arena)
static void* __cdecl stub_TireCreate(uint32_t a, uint32_t b, uint32_t c, const char* name) {
    L('TCRE'); L(a); L(b); L(c); LS(name);
    return g_tire;
}
static void __cdecl stub_TireDestroy(void* t) { L('TDES'); L(P(t)); }

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
using namespace mcar;
// tire_fn's and power_fn's arguments (hook/menu_car.cpp's TireArg / PowerArg)
struct HTireArg { void* tire; float load, camber; };
struct HPowerArg { void* curve; uint8_t power; uint8_t _5[3]; float limit, scale, rpm_scale; };

struct World {
    gxCanvas* screen;
    WidgetWindow* win;
    BalanceControl* bal;
    ChassisControl* ch;
    AlignControl* al;
    DrivetrainControl* dr;
    AeroControl* ae;
    FileControl* fi;
    HTireArg* targ;                                        // tire_fn's argument
    float* pts;                                           // shock_fn's
    HPowerArg* parg;                                       // power_fn's
    float* pcurve;
    uint8_t* setup2;                                      // a spare setup, a spare set
    uint8_t* set2;
    char *car, *dir, *title;
    uint8_t* flags;
    uint8_t* locale;
    char* scratch;                                        // a control's copy (the destructors)
    UIDialogItem* items;
};
static World W;

static UIDialogItem* it_(UIDialogItem* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h, const void* text,
                         int32_t i1c, const void* data, int32_t style) {
    memset(it, 0, sizeof *it);
    it->type = type; it->id = id; it->x = x; it->y = y; it->w = w; it->h = h; it->text = (const char*)text; it->i1c = i1c;
    it->data = (void*)data; it->style = style;
    return it + 1;
}
template <typename T> static T* ctl(uint32_t vt) {
    T* c = (T*)wv(sizeof(T));
    c->vtbl = (const void*)(uintptr_t)vt;
    return c;
}
static void* stamp(const char* n) { return fake_for(n, 'STMP'); }
// the original $E static initialisers of the given objects (from out/inventory.csv)
static int run_initialisers(const char* inv_path) {
    FILE* f = fopen(inv_path, "r");
    if (!f) { printf("can't open %s\n", inv_path); return 0; }
    char line[2048];
    int n = 0;
    static const char* objs[] = {",ui.obj,", ",_widget.obj,", ",widget.obj,", ",uistyle.obj,", ",mcar.obj,", ",locale.obj,", ",graph.obj,"};
    while (fgets(line, sizeof line, f)) {
        bool mine = false;
        for (const char* o : objs) mine |= strstr(line, o) != 0;
        if (!mine || !strstr(line, ",_$E")) continue;
        const uint32_t va = (uint32_t)strtoul(line, 0, 16);
        call_orig(va, 0, {});
        n++;
    }
    fclose(f);
    return n;
}
// every function-local Xlator the rewrites build on first use: {guard, bit, Xlator, key} (from hook/menu_car.cpp's xl_once calls)
static const uint32_t k_local_xl[][4] = {
    {0x0057a720, 0x01, 0x0057b268, 0x004f9510}, {0x0057a720, 0x02, 0x0057b728, 0x004f9520}, {0x0057a720, 0x04, 0x0057b318, 0x004f9534},
    {0x0057a720, 0x08, 0x0057b690, 0x004f9548}, {0x0057a720, 0x10, 0x0057b298, 0x004f9560}, {0x0057a720, 0x20, 0x0057a750, 0x004f9574},
    {0x0057a720, 0x40, 0x0057b548, 0x004f9588}, {0x0057a720, 0x80, 0x0057aec0, 0x004f959c}, {0x0057ae9c, 0x01, 0x0057ae90, 0x004f95b0},
    {0x0057ae9c, 0x02, 0x0057af50, 0x004f95c4}, {0x0057b264, 0x01, 0x0057a7a8, 0x004f9848}, {0x0057b264, 0x02, 0x0057a7b8, 0x004f986c},
    {0x0057b63c, 0x01, 0x0057af00, 0x004f97f4}, {0x0057b63c, 0x02, 0x0057b650, 0x004f980c}, {0x0057b63c, 0x04, 0x0057a728, 0x004f9814},
    {0x0057b63c, 0x08, 0x0057b2f8, 0x004f9820}, {0x0057b65c, 0x01, 0x0057a770, 0x004f974c}, {0x0057b65c, 0x02, 0x0057aef0, 0x004f9764},
    {0x0057b65c, 0x04, 0x0057af20, 0x004f9780}, {0x0057b65c, 0x08, 0x0057b778, 0x004f9788}, {0x0057b69c, 0x01, 0x0057b538, 0x004f989c},
    {0x0057b69c, 0x02, 0x0057b2a8, 0x004f98ac}, {0x0057b69c, 0x04, 0x0057b308, 0x004f98b8}, {0x0057b69c, 0x08, 0x0057b708, 0x004f98cc},
    {0x0057b69c, 0x10, 0x0057b338, 0x004f98e4}, {0x0057b69c, 0x20, 0x0057b660, 0x004f98fc}, {0x0057b69c, 0x40, 0x0057b328, 0x004f9914},
    {0x0057b69c, 0x80, 0x0057aeb0, 0x004f9928}, {0x0057b6bc, 0x01, 0x0057b2c8, 0x004f9940}, {0x0057b6bc, 0x02, 0x0057b738, 0x004f9958},
    {0x0057b6bc, 0x04, 0x0057af30, 0x004f9970}, {0x0057b6bc, 0x08, 0x0057aee0, 0x004f9988}, {0x0057b6bc, 0x10, 0x0057a740, 0x004f99a8},
    {0x0057b6bc, 0x20, 0x0057b640, 0x004f99c4}, {0x0057b6bc, 0x40, 0x0057b6c0, 0x004f99dc}, {0x0057b6bc, 0x80, 0x0057b2d8, 0x004f99f0},
    {0x0057b6dc, 0x01, 0x0057b2e8, 0x004f97a0}, {0x0057b6dc, 0x02, 0x0057af10, 0x004f97c4}, {0x0057b724, 0x01, 0x0057aed0, 0x004f95fc},
    {0x0057b724, 0x02, 0x0057af40, 0x004f960c}, {0x005d58fc, 0x01, 0x005d5900, 0x004fa1a4}, {0x005d58fc, 0x02, 0x005d5910, 0x004fa190},
    {0x005d58fc, 0x04, 0x005d5920, 0x004fa17c}, {0x005d592c, 0x01, 0x005d5940, 0x004fa098}, {0x005d592c, 0x02, 0x005d5950, 0x004fa160},
    {0x005d592c, 0x04, 0x005d5930, 0x004fa14c}, {0x005d598c, 0x01, 0x005d5960, 0x004f9f44}, {0x005d598c, 0x02, 0x005d5970, 0x004f9f38},
    {0x005d598c, 0x04, 0x005d5980, 0x004fa130}, {0x005d598c, 0x08, 0x005d5990, 0x004fa120}, {0x005d598c, 0x10, 0x005d59a0, 0x004fa108},
    {0x005d598c, 0x20, 0x005d59b0, 0x004fa0f4}, {0x005d598c, 0x40, 0x005d59c0, 0x004fa0dc}, {0x005d598c, 0x80, 0x005d59d0, 0x004fa0c0},
    {0x005d59ec, 0x01, 0x005d59f0, 0x004fa098}, {0x005d59ec, 0x02, 0x005d59e0, 0x004fa078}, {0x005d59fc, 0x01, 0x005d5a00, 0x004f9f44},
    {0x005d59fc, 0x02, 0x005d5a10, 0x004f9f38}, {0x005d59fc, 0x04, 0x005d5a20, 0x004fa00c}, {0x005d59fc, 0x08, 0x005d5a30, 0x004f9ff4},
    {0x005d59fc, 0x10, 0x005d5a40, 0x004f9fdc}, {0x005d59fc, 0x20, 0x005d5a50, 0x004f9fc4}, {0x005d59fc, 0x40, 0x005d5a60, 0x004f9fac},
    {0x005d5a6c, 0x01, 0x005d5a70, 0x004f9f84}, {0x005d5a6c, 0x02, 0x005d5a80, 0x004f9f6c}, {0x005d5a8c, 0x01, 0x005d5a90, 0x004f9f44},
    {0x005d5a8c, 0x02, 0x005d5aa0, 0x004f9f38}, {0x005d5a8c, 0x04, 0x005d5ab0, 0x004f9f24}, {0x005d5a8c, 0x08, 0x005d5ac0, 0x004f9f0c},
    {0x005d5a8c, 0x10, 0x005d5ad0, 0x004f9ef4}, {0x005d5a8c, 0x20, 0x005d5ae0, 0x004f9edc}, {0x005d5aec, 0x01, 0x005d5b50, 0x004f9ea4},
    {0x005d5aec, 0x02, 0x005d5b60, 0x004f9e88}, {0x005d5aec, 0x04, 0x005d5af0, 0x004f9e6c}, {0x005d5aec, 0x08, 0x005d5b00, 0x004f9e50},
    {0x005d5aec, 0x10, 0x005d5b10, 0x004f9e34}, {0x005d5aec, 0x20, 0x005d5b20, 0x004f9e18}, {0x005d5aec, 0x40, 0x005d5b40, 0x004f9dfc},
    {0x005d5aec, 0x80, 0x005d5b30, 0x004f9ddc}, {0x005d5b6c, 0x01, 0x005d5b70, 0x004f9da0}, {0x005d5b6c, 0x02, 0x005d5b80, 0x004f9d84},
    {0x005d5b6c, 0x04, 0x005d5b90, 0x004f9d6c}, {0x005d5b6c, 0x08, 0x005d5ba0, 0x004f9d50},
};
static void build_world(const char* inv_path) {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    HS->cf_mask = 0x1f;
    HS->file_size = 0x6ac;
    strcpy(HS->user_dir, "C:\\Users\\player\\");
    UI_G32(S_SCREEN_W) = 640;
    UI_G32(S_SCREEN_H) = 480;
    const int ne = run_initialisers(inv_path);
    call_orig(F_UIBegin, 0, {});
    // the function-local Xlators, as if every screen had been visited once (a guard bit set means its Xlator is built)
    for (auto& x : k_local_xl) {
        call_orig(F_Xlator_ctor, 1, {x[2], 0, x[3]});
        UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) | x[1]);
    }
    W.screen = (gxCanvas*)wv(sizeof(gxCanvas));
    W.screen->format = 4; W.screen->pixels = (uint8_t*)0x7b000000; W.screen->w = 640; W.screen->h = 480; W.screen->pitch = 1280;
    W.screen->cx1 = 640; W.screen->cy1 = 480;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    W.locale = (uint8_t*)wv(0x40);
    UI_GP(uint8_t, 0x00509354) = W.locale;
    W.car = wstr("viper");
    W.dir = wstr("track_setup");
    W.title = wstr("Viper GTS-R garage");
    W.flags = (uint8_t*)wv(8);
    W.setup2 = (uint8_t*)wv(0xd4);
    W.set2 = (uint8_t*)wv(0x6ac);
    W.scratch = (char*)wv(0x200);
    g_tire = (uint8_t*)wv(0x40);
    W.targ = (HTireArg*)wv(sizeof(HTireArg));
    W.pts = (float*)wv(16);
    W.parg = (HPowerArg*)wv(sizeof(HPowerArg));
    W.pcurve = (float*)wv(0x18);
    W.parg->curve = W.pcurve;
    // the controls, as MenuEditCar builds them
    W.bal = ctl<BalanceControl>(VT_BalanceControl);
    W.bal->stamp[0] = stamp("gbalance.stp"); W.bal->stamp[1] = stamp("gbalicon.stp");
    W.ch = ctl<ChassisControl>(VT_ChassisControl);
    W.ch->stamp[0] = stamp("ggraph.stp"); W.ch->stamp[1] = stamp("gshocks.stp"); W.ch->stamp[2] = stamp("gsprings.stp"); W.ch->page = 1;
    W.al = ctl<AlignControl>(VT_AlignControl);
    const char* an[6] = {"ggraph.stp", "gcamicon.stp", "gcamicor.stp", "gtoeicon.stp", "gtoeicor.stp", "gsidecar.stp"};
    for (int i = 0; i < 6; i++) W.al->stamp[i] = stamp(an[i]);
    W.dr = ctl<DrivetrainControl>(VT_DrivetrainControl);
    W.dr->stamp[0] = stamp("ggraph.stp"); W.dr->stamp[1] = stamp("ggears.stp"); W.dr->page = 6;
    W.dr->torque.vtbl = (const void*)(uintptr_t)VT_TorqueCurveControl; W.dr->torque.stamp = stamp("gtorque.stp");
    W.ae = ctl<AeroControl>(VT_AeroControl);
    W.ae->stamp[0] = stamp("ggraph.stp"); W.ae->stamp[1] = stamp("gaericon.stp");
    W.fi = ctl<FileControl>(VT_FileControl);
    W.fi->stamp = stamp("gsidecar.stp");
    // a car file, a setup and a set of plain values, so the controls' Added builds every page
    for (uint32_t a = M_CARFILE; a < M_CARFILE + 0x228; a += 4) UI_GF(a) = (float)((a >> 2) % 7) + 0.5f;
    UI_G32(M_CARFILE_108) = 6;
    UI_G32(M_57A7C4) = 6;
    for (uint32_t a = M_SETUP; a < M_SETUP + 0x8c; a += 4) UI_GF(a) = 0.5f;
    UI_G32(0x0057afd0) = 1;
    UI_GF(0x0057b254) = 1.0f;
    // the window: every control in it, added by the game's _UIAddItems and activated (their Create)
    UIDialogItem* it = W.items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 10);
    it = it_(it, 0x17, 0, 0x19a, 0x143, 0xbf, 0x36, "", 0, W.bal, 0);
    it = it_(it, 0x17, 0, 0x19a, 0x81, 0xbf, 0xbd, "", 0, W.ch, 0);
    it = it_(it, 0x17, 0, 0x19a, 0x81, 0xbf, 0xbd, "", 0, W.al, 0);
    it = it_(it, 0x17, 0, 0x19a, 0x81, 0xbf, 0xbd, "", 0, W.dr, 0);
    it = it_(it, 0x17, 0, 0x19a, 0x81, 0xbf, 0xbd, "", 0, W.ae, 0);
    it = it_(it, 0x17, 0, 0x28, 0x82, 0x208, 0xd2, "", 0, W.fi, 0);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    call_orig(F_WidgetCreateWindow, 0, {U(wstr("main_t.stp")), 0, 0, 640, 480, 1});
    W.win = (WidgetWindow*)(uintptr_t)g_r_eax;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    call_orig(F_UIAddItems, 0, {U(W.win), U(W.items)});
    call_orig(F_WW_Activate, 1, {U(W.win), 0});
    printf("world: %d static initialisers run, %d widgets in the garage window, %u KB of heap used\n", ne, W.win ? (int)W.win->count : -1,
           (HS->heap_next - A_HEAP) / 1024);
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
// a value that's usually a fraction (a setting), sometimes an edge: NaN, infinite, 0, -0, huge but formattable, tiny
static float setting() {
    if (chance(85)) return uni();
    switch (rnd() % 7) {
    case 0: return bitsf(0x7fc00000u);
    case 1: return (rnd() & 1) ? INFINITY : -INFINITY;
    case 2: return 0.0f;
    case 3: return -0.0f;
    case 4: return range(-1e5f, 1e5f);
    case 5: return bitsf(rnd() & 0x807fffffu);
    default: return range(-3.0f, 3.0f);
    }
}
static const char k_chars[] = "abcdefghij klmnopqrstuvwxyz ABCDEFGH 0123456789 .,!?-";
static void random_text(char* s, int maxlen) {
    const int n = irange(0, maxlen);
    for (int i = 0; i < n; i++) s[i] = k_chars[rnd() % (sizeof k_chars - 1)];
    s[n] = 0;
}
static void random_setup(uint8_t* s) {
    for (int i = 0; i < 0x8c; i += 4) { const float v = setting(); memcpy(s + i, &v, 4); }
    const int32_t kit = irange(0, 2);
    memcpy(s + 0x70, &kit, 4);
    const uint32_t ver = chance(90) ? 1 : rnd() % 3, size = chance(90) ? 0xd4 : 0xd0;
    memcpy(s + 0x8c, &ver, 4);
    memcpy(s + 0x90, &size, 4);
    memset(s + 0x94, 0, 0x40);
    random_text((char*)s + 0x94, 30);
}
// the car file's limit pairs (lo, hi) that the controls grey out when they're equal
static const uint32_t k_pairs[][2] = {
    {0x0057b14c, 0x0057b17c}, {0x0057b150, 0x0057b180}, {0x0057b154, 0x0057b184}, {0x0057b158, 0x0057b188},
    {0x0057b144, 0x0057b174}, {0x0057b148, 0x0057b178}, {0x0057b15c, 0x0057b18c}, {0x0057b160, 0x0057b190},
    {0x0057b16c, 0x0057b19c}, {0x0057b170, 0x0057b1a0}, {0x0057b164, 0x0057b194}, {0x0057b168, 0x0057b198},
    {0x0057b058, 0x0057b05c}, {0x0057b060, 0x0057b064}, {0x0057b100, 0x0057b104}, {0x0057b238, 0x0057b240},
    {0x0057b23c, 0x0057b244},
};
static void randomize_world() {
    // the setup, its loaded copy, the set
    random_setup((uint8_t*)(uintptr_t)M_SETUP);
    if (chance(15)) UI_G8(M_SETUP_NAME) = '*';
    if (chance(8)) UI_G8(M_SETUP_NAME) = 0;
    memcpy((void*)(uintptr_t)M_SETUP_ORIG, (const void*)(uintptr_t)M_SETUP, 0xd4);
    if (chance(50)) UI_G8(M_SETUP_ORIG + (rnd() % 0xd4)) ^= (uint8_t)(1 + rnd() % 255);
    UI_GU32(M_SET) = chance(90) ? 1 : 2;
    UI_GU32(M_SET + 4) = 0x6ac;
    UI_G32(M_SET_SLOT) = irange(0, 7);
    UI_GF(M_SLOT_F) = range(0, 7);
    for (int i = 0; i < 8; i++) random_setup((uint8_t*)(uintptr_t)(M_SET_SETUPS + 0xd4 * i));
    // the car file: limits and constants, some pairs equal; its gears (the first sometimes 0), their count
    for (uint32_t a = M_CARFILE; a < M_CARFILE + 0x228; a += 4) UI_GF(a) = chance(92) ? range(-2.0f, 60.0f) : setting();
    for (auto& p : k_pairs)
        if (chance(30)) UI_GU32(p[1]) = UI_GU32(p[0]);
    for (uint32_t a = 0x0057b120; a <= 0x0057b134; a += 4) UI_GF(a) = range(0.3f, 4.0f);
    if (chance(35)) UI_GF(0x0057b120) = chance(70) ? 0.0f : 1e-8f;
    UI_G32(M_CARFILE_108) = chance(85) ? irange(4, 6) : irange(-1, MENU_FIXES ? 6 : 9);   // (the fix build: never past 6)
    UI_G32(M_57A7C4) = chance(85) ? irange(4, 6) : irange(0, 8);
    if (chance(30)) UI_GF(0x0057b254) = 0.0f;
    if (chance(30)) UI_GF(0x0057b25c) = 0.0f;
    // the CarData (what CarFileCombine hasn't recomputed)
    for (uint32_t a = M_CARDATA; a < M_CARDATA + 0x1e0; a += 4) UI_GF(a) = chance(92) ? range(-3.0f, 3.0f) : setting();
    // colours
    for (uint32_t a : {0x0057a734u, 0x0057a7c8u, 0x0057aedcu, 0x0057af2cu, 0x0057b6acu, 0x0057a780u, 0x0057b6f0u}) UI_GU32(a) = rnd();
    if (chance(30)) UI_GU32(M_C_57B6AC) = UI_GU32(M_C_57AEDC);
    *(float*)(W.locale + 0x1c) = chance(90) ? range(0.5f, 2.0f) : setting();
    // the controls' pages (in their tables' range), the running window
    W.ch->page = irange(0, 6);
    W.al->page = irange(0, 9);
    W.dr->page = irange(0, 6);
    UI_GP(WidgetWindow, S_ACTIVE) = chance(92) ? W.win : 0;
    // the function-local statics' guards: mostly built, sometimes not
    static const uint32_t guards[] = {M_ONCE1, M_ONCE2, M_ONCE_FILE_CB, M_ONCE_SAVE, M_ONCE_LOAD_CB, M_ONCE_LOAD, M_ONCE_DEFAULT,
                                      M_ONCE_EXPORT1, M_ONCE_EXPORT2, M_ONCE_BALANCE, M_ONCE_CHASSIS_DRAW, M_ONCE_CHASSIS_ADDED,
                                      M_ONCE_ALIGN_DRAW, M_ONCE_ALIGN_ADDED, M_ONCE_DRIVE_ADDED, M_ONCE_AERO_ADDED,
                                      M_ONCE_FILE_DRAW, M_ONCE_FILE_ADDED};
    for (uint32_t g : guards) UI_G8(g) = chance(70) ? 0xff : (uint8_t)rnd();
    // the graph callbacks' arguments, the tire
    for (int i = 0; i < 16; i++) ((float*)g_tire)[i] = chance(90) ? range(0.1f, 3.0f) : setting();
    W.targ->tire = chance(90) ? g_tire : 0;
    W.targ->load = chance(90) ? range(0.0f, 2000.0f) : setting();
    W.targ->camber = chance(90) ? range(-10.0f, 10.0f) : setting();
    for (int i = 0; i < 4; i++) W.pts[i] = chance(90) ? range(0.0f, 1.0f) : setting();
    for (int i = 0; i < 6; i++) W.pcurve[i] = chance(90) ? range(-2.0f, 2.0f) : setting();
    W.parg->power = (uint8_t)(chance(50) ? 0 : rnd());
    W.parg->limit = chance(90) ? range(1000.0f, 9000.0f) : setting();
    W.parg->scale = chance(90) ? range(0.0f, 2.0f) : setting();
    W.parg->rpm_scale = chance(90) ? range(100.0f, 2000.0f) : setting();
    random_setup(W.setup2);
    memcpy(W.set2, (const void*)(uintptr_t)M_SET, 0x6ac);
    // the stubs' world
    HS->time = (int32_t)rnd();
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->cb_mask = (int32_t)rnd();
    HS->cb_calls = HS->idle_calls = HS->detect_calls = 0;
    HS->idle_exit_after = irange(1, 30);
    HS->detect_after = irange(1, 8);
    HS->grab_fail_at = chance(10) ? irange(1, 6) : -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->nfiles = 0;
    HS->file_open_mask = (int32_t)rnd();
    HS->cf_mask = chance(70) ? 0x1f : rnd();
    HS->file_size = chance(80) ? 0x6ac : irange(0, 0x800);
    HS->def_calls = 0;
    random_setup(HS->tmpl_setup);
    memcpy(HS->set_img, (const void*)(uintptr_t)M_SET, 0x6ac);
    if (chance(15)) ((uint32_t*)HS->set_img)[0] = 2;
    if (chance(15)) ((uint32_t*)HS->set_img)[1] = 0x6a0;
    for (int i = 0; i < 8; i++) {
        uint8_t* s = HS->set_img + 0xc + 0xd4 * i;
        random_setup(s);
    }
    HS->script_n = HS->script_pos = 0;
}
// a random input script (for the functions that run a dialog)
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x123, 0x124, 0x125, 0x127, 0x12e, 0x126, 0x128, 9, 'q', 'a', 'Z', '5', 0x141};
    const int n = irange(0, maxops);
    for (int i = 0; i < n && i < 62; i++) {
        int32_t* op = HS->script[i];
        const uint32_t r = rnd() % 10;
        op[1] = op[2] = 0;
        if (r < 4) { op[0] = OP_MOVE_W; op[1] = irange(0, 80); op[2] = irange(0, 479); }
        else if (r < 5) { op[0] = OP_MOVE_XY; op[1] = irange(-10, 650); op[2] = irange(-10, 490); }
        else if (r < 6) op[0] = OP_DOWN;
        else if (r < 7) op[0] = OP_UP;
        else if (r < 9) { op[0] = OP_KEY; op[1] = keys[rnd() % (sizeof keys / sizeof keys[0])]; }
        else op[0] = OP_NOP;
        if (op[0] == OP_MOVE_W && chance(70)) {            // a click: a move, a press and a release
            HS->script[i + 1][0] = OP_DOWN; HS->script[i + 2][0] = OP_UP;
            i += 2;
        }
        HS->script_n = i + 1;
    }
}

// ---- poison: .data/.bss random but for what's read as a string or followed as a pointer, and the garage's values ------------------
static void poison(const Mem& keepfrom) {
    uint32_t* d = (uint32_t*)DATA;
    for (uint32_t i = 0; i < DATA_BYTES / 4; i++) d[i] = rnd();
    static const uint32_t keep[][2] = {
        {0x004e1000, 0x004e6000},             // the library's constants and strings before mcar.obj's
        {0x004eb108, 0x004eb10c},             // Xlator::g_cookie
        {0x004efbf8, 0x004efbfc},             // the current canvas
        {0x004f6658, 0x004f7300},             // the UI library's data (the style descriptions, strings, cursors)
        {0x004f9278, 0x004fa1c0},             // mcar.obj's data: its globals and strings
        {0x005024b8, 0x005024bc},             // __adjust_fdiv
        {0x00503d00, 0x00503e00},             // graph.obj's data (the axes' number formats)
        {0x00509354, 0x00509358},             // the locale
        {0x005228d4, 0x005228d8}, {0x005228f4, 0x005228f8},   // the screen's size
        {0x00578cd0, 0x00579790},             // the UI library's .bss (the windows, the styles, the Xlators)
        {0x0057a720, 0x0057b790},             // mcar.obj's .bss: the setup, the set, the car file, the CarData, the Xlators
        {0x005d58f0, 0x005d5bb0},             // the controls' function-local Xlators and guards
    };
    for (auto& k : keep) memcpy(DATA + (k[0] - 0x004e1000), keepfrom.data + (k[0] - 0x004e1000), k[1] - k[0]);
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static McCtl* control(const char* cls) {
    if (!strcmp(cls, "BalanceControl")) return W.bal;
    if (!strcmp(cls, "ChassisControl")) return W.ch;
    if (!strcmp(cls, "AlignControl")) return W.al;
    if (!strcmp(cls, "DrivetrainControl")) return W.dr;
    if (!strcmp(cls, "TorqueCurveControl")) return &W.dr->torque;
    if (!strcmp(cls, "AeroControl")) return W.ae;
    if (!strcmp(cls, "FileControl")) return W.fi;
    return 0;
}
static bool is_modal(const char* nm) {
    return !strcmp(nm, "MenuEditCar") || strstr(nm, "_cb") || strstr(nm, "_dlg");
}
static void fill_random(void* p, uint32_t n) { uint8_t* b = (uint8_t*)p; for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)rnd(); }
// the arguments for f (words: ecx, edx first for a __thiscall); false: skip this round
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const char* nm = f.name;
    uint32_t* a = f.fast ? w + 2 : w;
    char cls[64] = "";
    const char* meth = nm;
    if (const char* sep = strstr(nm, "::")) {
        const size_t n = (size_t)(sep - nm) < 63 ? (size_t)(sep - nm) : 63;
        memcpy(cls, nm, n);
        cls[n] = 0;
        meth = sep + 2;
    }
    if (strstr(nm, "destructor helper")) return true;
    if (McCtl* c = control(cls)) {
        w[0] = U(c);
        if (strstr(meth, "deleting destructor")) {
            memcpy(W.scratch, c, ctl_size(c));                 // a copy: its stamps forgotten, perhaps freed
            w[0] = U(W.scratch);
            a[0] = rnd() & 3;
        } else if (!strcmp(meth, "Callback")) {
            a[0] = (uint32_t)irange(-2, 9);
            if ((c == W.ch && (int32_t)a[0] > 6) || (c == W.al && (int32_t)a[0] > 9) || (c == W.dr && (int32_t)a[0] > 6)) a[0] = 1;
            a[1] = rnd();
        } else if (!strcmp(meth, "Draw")) {
            const Widget* cw = (const Widget*)c->widget;
            a[0] = cw && cw->window && chance(60) ? U(&((WidgetWindow*)cw->window)->canvas) : U(W.screen);
        }
        return true;
    }
#define IS(n) (!strcmp(nm, n))
    if (IS("get_gear")) a[0] = (uint32_t)irange(-1, 8);
    else if (IS("tire_fn")) { a[0] = fbits(chance(85) ? range(-12.0f, 12.0f) : setting()); a[1] = U(W.targ); }
    else if (IS("shock_fn")) { a[0] = fbits(chance(85) ? range(-3.0f, 3.0f) : setting()); a[1] = U(W.pts); }
    else if (IS("power_fn")) { a[0] = fbits(chance(85) ? range(0.0f, 8.0f) : setting()); a[1] = U(W.parg); }
    else if (IS("export_setup")) { a[0] = U(W.car); a[1] = U(W.dir); a[2] = chance(70) ? M_SETUP : U(W.setup2); }
    else if (IS("load_setup_set") || IS("save_setup_set")) { a[0] = U(W.car); a[1] = U(W.dir); a[2] = chance(70) ? M_SET : U(W.set2); }
    else if (IS("MenuEditCar")) { a[0] = U(W.car); a[1] = U(W.dir); a[2] = U(W.flags); a[3] = chance(60) ? U(W.title) : 0; }
    else if (strstr(nm, "_cb")) a[0] = rnd();
#undef IS
    return true;
}

#if MENU_FIXES
// ---- the fixes, each on its bad case (directed_fix_tests) -----------------------------------------------------------------
// The rewrite alone on the case the original gets wrong (it would overrun this program's stack, or write past the setup),
// from the pristine world with the case set up: it must return cleanly (no fault, the bytes popped, ebx / esi / edi / ebp
// kept) and give what the fix promises. Nothing written outside: each is compared, all memory and the call logs, with a
// reference run of the ORIGINAL on the nearest state it gets right (the same name ending inside its buffer, six gears, a
// short car name), which is what the fix makes of the bad case; the bytes the fix leaves alone past the setup are guard
// bytes, checked unchanged. (The faithful rewrites fail them, or bring this program down.)
static int g_fx_bad, g_fx_n;
static const Ent& fx_fn(const char* name) {
    for (int i = 0; i < g_nfns; i++)
        if (!strcmp(g_fns[i].name, name)) return g_fns[i];
    printf("  fix test: %s isn't listed\n", name);
    fflush(stdout);
    ExitProcess(4);
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
    if (where) printf(" (first difference at %08x)", where);
    printf("\n");
}
// the log holds `pre` followed by the string s as LS logs it
static bool fx_logged(std::initializer_list<uint32_t> pre, const char* s) {
    std::vector<uint32_t> want(pre);
    uint32_t w = 0;
    const int n = (int)strlen(s);
    for (int i = 0; i < n; i++) {
        w = w << 8 | (uint8_t)s[i];
        if ((i & 3) == 3) { want.push_back(w); w = 0; }
    }
    want.push_back(w);
    want.push_back((uint32_t)n);
    const uint32_t ln = g_log.n < LOG_MAX ? g_log.n : LOG_MAX;
    for (uint32_t i = 0; i + want.size() <= ln; i++)
        if (!memcmp(&g_log.w[i], want.data(), 4 * want.size())) return true;
    return false;
}
static bool fx_same_log() {
    return g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
}
static void fx_keep_log() { memcpy(&g_log_orig, &g_log, sizeof(uint32_t) * (1 + (g_log.n < LOG_MAX ? g_log.n : LOG_MAX))); }
static void fx_script(std::initializer_list<uint16_t> keys) {
    int i = 0;
    for (uint16_t k : keys) { HS->script[i][0] = OP_KEY; HS->script[i][1] = k; HS->script[i][2] = 0; i++; }
    HS->script_n = i; HS->script_pos = 0; HS->frames = 0; HS->frame_limit = 40;
}
static void fx_name(uint32_t at, char c, int n, bool end) {   // n characters c at `at`, then a terminator if `end`
    for (int i = 0; i < n; i++) UI_G8(at + (uint32_t)i) = (uint8_t)c;
    if (end) UI_G8(at + (uint32_t)n) = 0;
}
static char g_fx_car[0x200];                                  // MenuEditCar's car name (this program's memory: not compared)

static int directed_fix_tests() {
    Result r;
    uint32_t o;
    char m[200];
    // ---- 1. save_setup_dlg: the name field, and the setup's name copied into it ----
    {
        const Ent& f = fx_fn("save_setup_dlg");
        // (a) a name typed to the field's limit (40 characters there, 30 more typed: the field stops at 63), OK: the
        // original's bytes, which stay inside its frame
        mem_load(g_pristine);
        fx_name(M_SETUP_NAME, 'n', 40, true);
        fx_script({'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q', 'q',
                   'q', 'q', 'q', 'q', 'q', 0x124, 0xd});
        mem_save(g_snap);
        const Result ro = fx_run(f, false, {});
        mem_save(g_after);
        fx_keep_log();
        mem_load(g_snap);
        r = fx_run(f, true, {});
        fx_check(fx_clean(f, r), "save_setup_dlg, a name typed to the field's limit: a clean return", &r);
        char want[80];
        memset(want, 'n', 40);
        memset(want + 40, 'q', 23);
        want[63] = 0;
        fx_check(!memcmp((const void*)(uintptr_t)M_SETUP_NAME, want, 64), "save_setup_dlg, a name typed to the field's limit: the 63 characters saved");
        o = mem_diff(g_after);
        fx_check(!ro.fault && ro.ret == r.ret && o == 0 && fx_same_log(), "save_setup_dlg, a name typed to the field's limit: the original's result", 0, o);
        // (b) the setup's name with no end in its 64 bytes (and 300 more bytes of it past the setup), OK: the reference is
        // the original on the same name ending at its 63rd character
        for (int star = 0; star < 2; star++) {
            mem_load(g_pristine);
            fx_name(M_SETUP_NAME, 'n', 0x40, false);
            if (star) UI_G8(M_SETUP_NAME) = '*';
            fx_name(M_SETUP + 0xd4, 'x', 300, true);
            fx_script({0xd});
            mem_save(g_snap);
            UI_G8(M_SETUP_NAME + 0x3f) = 0;                        // the reference: the name ends inside the setup
            if (star) {                                            // ('*' first: the name after it, 63 characters, ends
                fx_name(M_SETUP + 0xd4, 'x', 300, true);           //  in the byte after the setup in the reference)
                UI_G8(M_SETUP + 0xd4) = 0;
                UI_G8(M_SETUP_NAME + 0x3f) = 'n';
            }
            const Result rr = fx_run(f, false, {});
            if (star) UI_G8(M_SETUP + 0xd4) = 'x';
            mem_save(g_after);
            fx_keep_log();
            mem_load(g_snap);
            r = fx_run(f, true, {});
            sprintf(m, "save_setup_dlg, the setup's name with no end%s: a clean return", star ? " ('*' first)" : "");
            fx_check(fx_clean(f, r) && r.ret == 1, m, &r);
            memset(want, 'n', 63);
            want[63] = 0;
            sprintf(m, "save_setup_dlg, the setup's name with no end%s: its first 63 characters saved", star ? " ('*' first)" : "");
            fx_check(!memcmp((const void*)(uintptr_t)M_SETUP_NAME, want, 64), m);
            o = mem_diff(g_after);
            sprintf(m, "save_setup_dlg, the setup's name with no end%s: as the original on the name cut to 63", star ? " ('*' first)" : "");
            fx_check(!rr.fault && rr.ret == r.ret && o == 0 && fx_same_log(), m, 0, o);
        }
    }
    // ---- 2. DrivetrainControl::Draw: a car file of more than 6 gears ----
    {
        const Ent& f = fx_fn("DrivetrainControl::Draw");
        for (int32_t count : {7, 9, 64, 0x40000001, 0x7fffffff}) {
            // every float from the ratios to the end of the setup, and the 4 bytes after it, 1.0: all within 0.04 of each
            // other, so every one the loop reaches is raised
            mem_load(g_pristine);
            for (uint32_t a = 0x0057afb0; a < M_SETUP + 0xd8; a += 4) UI_GF(a) = 1.0f;
            UI_G32(M_57A7C4) = 6;
            UI_G32(M_CARFILE_108) = 6;                           // the reference: six gears
            mem_save(g_snap);
            const Result rr = fx_run(f, false, {U(W.dr), 0, U(W.screen)});
            mem_save(g_after);
            fx_keep_log();
            // (the original: is the case real? A count past 2^28 wraps its address and walks down through the game's
            // code and data, so only the smaller ones are run)
            bool real = true;
            if (count <= 64) {
                mem_load(g_snap);
                UI_G32(M_CARFILE_108) = count;
                const Result rb = fx_run(f, false, {U(W.dr), 0, U(W.screen)});
                UI_G32(M_CARFILE_108) = 6;
                real = rb.fault || mem_diff(g_after) != 0;
            }
            mem_load(g_snap);
            UI_G32(M_CARFILE_108) = count;
            r = fx_run(f, true, {U(W.dr), 0, U(W.screen)});
            sprintf(m, "DrivetrainControl::Draw, %d gears: a clean return", count);
            fx_check(fx_clean(f, r), m, &r);
            UI_G32(M_CARFILE_108) = 6;
            o = mem_diff(g_after);
            sprintf(m, "DrivetrainControl::Draw, %d gears: as the original with 6 (nothing past the six ratios written)", count);
            fx_check(!rr.fault && o == 0 && fx_same_log(), m, 0, o);
            sprintf(m, "DrivetrainControl::Draw, %d gears: the original reads or writes past the six (the case is real)", count);
            fx_check(real, m);
        }
    }
    // ---- 3. MenuEditCar: a long car name; the '*' before a long setup name ----
    {
        const Ent& f = fx_fn("MenuEditCar");
        // the set's file opens (so load_setup_set never builds its path from the car's name: its own FIX CANDIDATE)
        char path[128];
        sprintf(path, "%ssetups\\%s.set", HS->user_dir, W.dir);
        const int32_t open_mask = (hash_str(path) & 1) ? 0 : 1;
        for (int len : {28, 31, 100, 235, 236, 300}) {
            // the reference: the original on "viper" at the same address (the name only reaches the stubs' log)
            mem_load(g_pristine);
            HS->file_open_mask = open_mask;
            fx_script({0x1b});
            strcpy(g_fx_car, "viper");
            mem_save(g_snap);
            const Result rr = fx_run(f, false, {U(g_fx_car), U(W.dir), U(W.flags), U(W.title)});
            mem_save(g_after);
            mem_load(g_snap);
            memset(g_fx_car, 'c', (size_t)len);
            g_fx_car[len] = 0;
            for (int i = 0; i < len; i += 7) g_fx_car[i] = (char)('A' + (i / 7) % 26);
            r = fx_run(f, true, {U(g_fx_car), U(W.dir), U(W.flags), U(W.title)});
            sprintf(m, "MenuEditCar, a %d-character car name: a clean return", len);
            fx_check(fx_clean(f, r), m, &r);
            char res[0x200], cf[0x200];
            const int keep = len < 0xeb ? len : 0xeb;
            sprintf(res, "%.*s.car", keep, g_fx_car);
            sprintf(cf, "%.*s.cf", keep, g_fx_car);
            sprintf(m, "MenuEditCar, a %d-character car name: \"<car>.car\" loaded and unloaded, \"<car>.cf\" loaded (%s)", len,
                    len > 0xeb ? "cut to 235" : "whole");
            fx_check(fx_logged({'RSML'}, res) && fx_logged({'RSUL'}, res) && fx_logged({'CFLF', M_CARFILE}, cf), m);
            o = mem_diff(g_after);
            sprintf(m, "MenuEditCar, a %d-character car name: memory as the original's with a short name", len);
            fx_check(!rr.fault && rr.ret == r.ret && o == 0, m, 0, o);
        }
        // the '*' of an unsaved setup before a 62- and a 63-character name (loaded as '*' + the name, so the garage
        // strips it, and puts it back on the way out, the setup being unsaved); the 4 bytes after the setup guard bytes
        for (int len : {61, 62, 63}) {
            mem_load(g_pristine);
            HS->file_open_mask = open_mask;
            strcpy(g_fx_car, "viper");
            uint8_t* t = HS->tmpl_setup;
            memset(t, 0, 0xd4);
            t[0x94] = '*';
            memset(t + 0x95, 'm', (size_t)len);                   // 63: no terminator in the setup (the byte after it ends it)
            UI_G8(M_SETUP + 0xd4) = len == 63 ? 0 : 0xee;
            UI_G8(M_SETUP + 0xd5) = 0xee; UI_G8(M_SETUP + 0xd6) = 0xee; UI_G8(M_SETUP + 0xd7) = 0xee;
            fx_script({0x1b});
            mem_save(g_snap);
            const Result ro = fx_run(f, false, {U(g_fx_car), U(W.dir), U(W.flags), U(W.title)});
            const bool real = UI_G8(M_SETUP + 0xd4) != (len == 63 ? 0 : 0xee) || UI_G8(M_SETUP + 0xd5) != 0xee;
            mem_save(g_after);
            fx_keep_log();
            mem_load(g_snap);
            r = fx_run(f, true, {U(g_fx_car), U(W.dir), U(W.flags), U(W.title)});
            sprintf(m, "MenuEditCar, '*' before a %d-character setup name: a clean return", len);
            fx_check(fx_clean(f, r), m, &r);
            char want[64];
            want[0] = '*';
            const int kept = len < 62 ? len : 62;
            memset(want + 1, 'm', (size_t)kept);
            want[1 + kept] = 0;
            sprintf(m, "MenuEditCar, '*' before a %d-character setup name: '*' and %d characters, ending inside the setup", len, kept);
            fx_check(!memcmp((const void*)(uintptr_t)M_SETUP_NAME, want, (size_t)(2 + kept)), m);
            sprintf(m, "MenuEditCar, '*' before a %d-character setup name: the 4 bytes after the setup untouched", len);
            fx_check(UI_G8(M_SETUP + 0xd4) == (len == 63 ? 0 : 0xee) && UI_G8(M_SETUP + 0xd5) == 0xee && UI_G8(M_SETUP + 0xd6) == 0xee &&
                     UI_G8(M_SETUP + 0xd7) == 0xee, m);
            // the original's result with what the fix changes put back (the bytes after the setup; a 63-character name's
            // last character): the rest the same
            uint8_t* a = g_after.data + (M_SETUP - 0x004e1000);
            a[0xd4] = len == 63 ? 0 : 0xee; a[0xd5] = a[0xd6] = a[0xd7] = 0xee;
            if (len == 63) a[0x94 + 0x3f] = 0;
            o = mem_diff(g_after);
            sprintf(m, "MenuEditCar, '*' before a %d-character setup name: otherwise the original's result%s", len,
                    len == 63 ? "" : " (and its log)");
            fx_check(!ro.fault && ro.ret == r.ret && o == 0 && (len == 63 || fx_same_log()), m, 0, o);
            sprintf(m, "MenuEditCar, '*' before a %d-character setup name: the original writes past the setup%s", len,
                    len == 61 ? " (it doesn't: the fix leaves this one alone)" : " (the case is real)");
            fx_check(len == 61 ? !real : real, m);
        }
    }
    printf("directed fix tests: %s (%d checks, %d failed)\n", g_fx_bad ? "FAILED" : "all passed", g_fx_n, g_fx_bad);
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
    const bool verbose = GetEnvironmentVariableA("VP_VERBOSE", 0, 0) != 0;
    char exe[MAX_PATH], inv[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_menu_car.cpp");
    if (s) *s = 0;
    sprintf(inv, "%s\\out\\inventory.csv", exe);
    strcat(exe, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    GetCurrentThreadStackLimits(&g_stack_lo, &g_stack_hi);
    {
        HMODULE m = GetModuleHandleA(0);
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
        g_self_lo = (uintptr_t)m; g_self_hi = (uintptr_t)m + nt->OptionalHeader.SizeOfImage;
        g_code_lo = (uintptr_t)m + nt->OptionalHeader.BaseOfCode;
        g_code_hi = g_code_lo + nt->OptionalHeader.SizeOfCode;
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
        // the garage's
        {F_Win32GetUserDirectory, (void*)&stub_UserDir}, {F_ResourceSetMustLoad, (void*)&stub_ResMust},
        {F_ResourceSetUnload, (void*)&stub_ResUnload}, {F_CarFileLoadSetup, (void*)&stub_CarFileLoadSetup},
        {F_CarFileLoad_file, (void*)&stub_CarFileLoadFile}, {F_CarFileLoad_data, (void*)&stub_CarFileLoadData},
        {F_CarFileSaveSetup, (void*)&stub_CarFileSaveSetup}, {F_CarFileLoadDefaultSetup, (void*)&stub_CarFileLoadDefaultSetup},
        {F_FileSize, (void*)&stub_FileSize}, {F_FileReadExact, (void*)&stub_FileReadExact}, {F_FileCreate, (void*)&stub_FileCreate},
        {F_FileWrite, (void*)&stub_FileWrite}, {F_FileCreateDirectory, (void*)&stub_FileCreateDirectory},
        {F_HTMLBegin, (void*)&stub_HTMLBegin}, {F_HTMLWrite, (void*)&stub_HTMLWrite}, {F_HTMLWriteLn, (void*)&stub_HTMLWriteLn},
        {F_HTMLEnd, (void*)&stub_HTMLEnd}, {F_TireCreate, (void*)&stub_TireCreate}, {F_TireDestroy, (void*)&stub_TireDestroy},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(0, 0, watchdog_thread, 0, 0, 0);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    build_world(inv);
    mem_save(g_pristine);

    int dup = 0;
    for (int i = 0; i < g_nfns; i++)
        for (int j = i + 1; j < g_nfns; j++)
            if (g_fns[i].v10 == g_fns[j].v10 || !strcmp(g_fns[i].name, g_fns[j].name)) {
                printf("  listed twice: %08x %s / %08x %s\n", g_fns[i].v10, g_fns[i].name, g_fns[j].v10, g_fns[j].name);
                dup++;
            }

#if MENU_FIXES
    const int fix_bad = directed_fix_tests();
#else
    const int fix_bad = 0;
#endif

    static Footprint fp;
    long long checks = 0, poisoned_checks = 0, log_words = 0, both_fault = 0, both_hung = 0, faults_loaded = 0, faults_poisoned = 0;
    int differ = 0, fp_bad = 0, faults = 0, pure_n = 0, replay_n = 0, changed_fns = 0, still_fns = 0, bad_fns = 0, skipped = 0;
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        g_fn_lo = f.v10;
        g_fn_hi = f.v10 + 0xd00;                          // (MenuEditCar, the longest, is 0xcd0 bytes)
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0;
        const int nr = is_modal(f.name) ? rounds : rounds * 10;
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            randomize_world();
            if (poisoned) poison(g_pristine);
            random_script(is_modal(f.name) ? 24 : 4);
            uint32_t words[72];
            if (!make_args(f, words)) { skipped++; continue; }
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            mem_save(g_snap);
            if (verbose) printf("    round %d: the original\n", rd);
            const Result ro = run(f, false, words);
            const bool changed = mem_diff(g_snap) != 0;
            fn_changed |= changed;
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
            if (verbose) printf("    round %d: the rewrite (the original %s %08x at %08x, %u log words)\n", rd, ro.fault ? "faulted" : "returned",
                                ro.code, ro.eip, g_log_orig.n);
            if (verbose && ro.fault && ro.code == 0xe0000d06u) {
                printf("     hung: the log's last words:");
                const uint32_t n = g_log_orig.n < LOG_MAX ? g_log_orig.n : LOG_MAX;
                for (uint32_t k = 0; k < n && k < 3000; k++)
                    printf(((g_log_orig.w[k] >> 24) >= 0x41 && (g_log_orig.w[k] >> 24) <= 0x5a) ? "\n %08x" : " %08x", g_log_orig.w[k]);
                printf("\n");
            } else if (verbose && ro.fault) {
                printf("     the log:");
                for (uint32_t k = 0; k < g_log_orig.n && k < 400; k++) {
                    const uint32_t v = g_log_orig.w[k];
                    const bool tag = ((v >> 24) >= 'A' && (v >> 24) <= 'Z');
                    printf(tag ? "\n      %c%c%c%c" : " %08x", tag ? (int)(v >> 24) : v, (int)((v >> 16) & 0xff), (int)((v >> 8) & 0xff), (int)(v & 0xff));
                }
                printf("\n");
            }
            const Result rn = run(f, true, words);
            checks++;
            fn_checks++;
            poisoned_checks += poisoned;
            log_words += g_log_orig.n;
            if (ro.fault || rn.fault) { faults++; fn_faults++; }
            if (ro.fault && rn.fault) both_fault++;
            if (ro.fault || rn.fault) (poisoned ? faults_poisoned : faults_loaded)++;
            bool same = ro.fault == rn.fault && ro.code == rn.code && ro.ret == rn.ret && ro.pops == rn.pops && ro.top == rn.top &&
                        !memcmp(&ro.st0, &rn.st0, sizeof ro.st0) && !memcmp(ro.regs, rn.regs, sizeof ro.regs);
            const bool hung = ro.fault && rn.fault && ro.code == 0xe0000d06u && rn.code == 0xe0000d06u;
            const uint32_t where = hung ? 0 : mem_diff(g_after);
            same &= where == 0;
            if (hung) {
                // both stuck in the same endless frame loop (a poisoned world): the logs up to where the shorter stops (or
                // the log's capacity) must match; how far each got in 2 s, and the memory, can't be compared
                both_hung++;
                const uint32_t m = g_log.n < g_log_orig.n ? g_log.n : g_log_orig.n;
                same = !memcmp(g_log.w, g_log_orig.w, 4 * (m < LOG_MAX ? m : LOG_MAX));
            } else {
                same &= g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
            }
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
                        for (uint32_t k = i >= 40 ? i - 40 : 0; k < i + 6 && k < g_log.n; k++) printf(" %08x/%08x", g_log_orig.w[k], g_log.w[k]);
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
           "compared: %d differ, %d footprint violations, %d checks faulted (%lld in both, the same way; %lld of those both stuck in "
           "an endless frame loop -- poisoned .data --, compared as far as the shorter log), %d skipped; %d functions bad\n",
           g_nfns, pure_n, replay_n, g_nfns - pure_n - replay_n, dup, checks, poisoned_checks, log_words, differ, fp_bad, faults,
           both_fault, both_hung, skipped, bad_fns);
    printf("faults by round kind: %lld as loaded, %lld poisoned; %lld dwords holding a return address into the function under "
           "test (a stock dialog's copy of its frame) taken as equal\n", faults_loaded, faults_poisoned, ret_pairs);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    return differ || fp_bad || dup || fix_bad ? 1 : 0;
}
