// world_menu_options.cpp -- M3 UI stage, step U2 (group B): every rewrite of hook/menu_options.cpp (moptions.obj,
// mmixer.obj), hook/menu_race.cpp (mrace.obj) and the generated hook/menu_leftover.cpp (the $E static initialisers of
// all eleven menu object files, and moptions / mrace / mmixer's stubs and deleting destructors) against its original,
// outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_menu_options.cpp
//        /Fo<dir>\ /Fe<dir>\world_menu_options.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_menu_options.exe [rounds] [seed]      (VP_TRACE=1: one line per function; VP_ONLY=name: just those)
//
// Built as test/world_ui.cpp is (its loader, raw call, memory comparison, stubs and logs are copied here): out\race_v10.exe
// loaded at 0x400000, the three rewrite files included with PORT_FN redefined to list each function (its v1.0 address,
// the rewrite, its calling convention, stack arguments, return, footprint).
//
// The world. Once, at start: every $E static initialiser of the libraries `ui` and `menu` (the Xlators, the colours; read
// from out\inventory.csv), the game's own UIBegin (the styles; fonts and stamps from the stubs), a full-screen window made
// by WidgetCreateWindow and made the running one, and in it -- by the game's own _UIAddItems -- the five Options tabs
// (GfxOptionsControl, SoundOptionsControl with its MixerChooser, ControlsOptionsControl with its ControlTestControl,
// AidsOptionsControl, HackOptionsControl), each built by its ORIGINAL constructor, and adjust_ctls's three bars (two
// Meters, a SteerMeter) built as adjust_ctls builds them. Their Addeds (the originals) fill the window with their
// widgets. This is the pristine state.
//
// Then for EVERY listed function, `rounds` times (default 24; ten times that for a function that neither runs a dialog nor
// is a static initialiser): pristine restored; half the rounds POISONED (every byte of .data/.bss random, except what's
// read as a string or followed as a pointer: every dword the pristine state holds that points into the image, the arena
// or this program, the strings of the ui and menu object files, the dialog static at 0x4f8100, the screen's size,
// __adjust_fdiv); the controls' values random (floats sometimes NaN / infinite / huge; the Sound tab's fidelity kept to
// 0..1, which is all its slider and its constructor ever leave -- FIX CANDIDATE in hook/menu_options.cpp), the widgets'
// flags random, the stubs' world random (the options present or missing and their values, the driver's readings and
// digital flags, the mixers, the cars and tracks, the joystick's name, the benchmark, the replay loader); arguments made
// for the function (the right object for a method, a canvas, a callback's id, a deleting destructor's flags, fresh random
// blocks for the constructors, out buffers); for a function that runs a dialog (MenuDoOptions, MenuDoRaceMenu,
// MenuDoRaceSetup, adjust_ctls, tune_default) a random input script -- mouse moves onto the running window's widgets,
// presses and releases, keys, idle frames -- ended by a watchdog after 40 frames if the dialog doesn't end itself.
// Snapshot; the stack below is filled with one pattern (so a local the original leaves uninitialised -- the Sound tab's
// fidelity, the frames' unused bytes -- reads the same in both passes); the ORIGINAL runs from a raw call thunk (ecx /
// edx / the stack set exactly; eax, st0, the x87 stack depth, the bytes popped, ebx / esi / edi / ebp recorded); its
// results kept, the snapshot restored, the REWRITE runs the same way. Compared: every byte of .data/.bss/.idata and the
// 1 MB arena (a dword where both passes left a pointer into this thread's stack counts as equal: each pass's frames
// differ), the return value, the call logs of every stub, faults. Every byte the original changed must lie in the
// rewrite's footprint (unless it's replay_only; the stubs' own state block excepted); a pure one changes nothing. The x87
// runs at 24 or 53 bits (alternating rounds). Built as above, VP_FAITHFUL: every rewrite must match its original bit
// for bit.
//
// Built with /DVP_MENU_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Menus": in
// these files, MenuDoRaceMenu's default name and MenuDoRaceSetup's random track). Every function is still compared as
// above, on states kept clear of the fixed cases: the track count is never 2 (the faithful build's 3% of rounds with two
// tracks get 3..9), and the default player name's translation (Main_Menu:DefaultPlayerName, whose text this world's
// Xlator::xlate otherwise gives as the 27-character key itself, so every faithful MenuDoRaceMenu round runs the overrun)
// is a name that fits its 16 bytes: "Player" (the English), "" or one of exactly 15 characters. Then directed_fix_tests:
// each fix's bad case on the rewrite -- no fault, the bytes popped and ebx / esi / edi / ebp kept, and the original's
// result where it has a sensible one: the rewrite given a long name (16 to 4095 characters, a saved name present or
// missing) against the original given its first 15 characters, and the rewrite with two tracks (where the original faults
// dividing by zero, checked) against the original with one (t % -1 == 0, row 0); every byte of .data/.bss/.idata and the
// arena and the call logs compared, the outputs' buffers pre-filled so a byte past the string shows.
//
// Stubbed (a jump to a logger; state in the arena so both passes see the same): world_ui.cpp's (MemAlloc / delete, the
// logs, the input and the clock, the 2D calls, the fonts and stamps, the palettes, Xlator::xlate, sprintf, the controls'
// detection, the files, atexit, the CRT's fatal paths) and this step's callees outside the menus: the options
// (OptionsGet / OptionsSet of every type -- a Get finds its key or not, by a hash of the key and the round, and a missing
// one logs the value the caller holds --, OptionsLoadModule, OptionsFlush), ControlGet / ControlSet, the driver
// (DriverBegin / End / Refresh / Update, the readings, the digital flags), the sound (SoundRestart, Sound::Toss, the
// mixers -- MixerGet hands out fake IMixers whose GetName / GetCaps / IsNullMixer log --, the volume and quality), the
// front end's lists (the cars, the tracks, HackGetCarIndex), VersionGetBuildString, JoyGetName, ReplayBenchmark,
// ReplayLoadDialog, PaintKitDo, and group C's classes that MenuDoRaceSetup builds (TrackChooser, CarChooser,
// OpponentViewer, RaceOptionViewer: their constructors give the object a logging UICustomControl vtable; their
// destructors, SetCar, TrackViewer::GetTrackName and CarViewer3D::GetName log). The game's own code runs everywhere else:
// the whole widget toolkit (UIDoDialog, _UIAddItems, the widgets, UIDoYesNoBox, CreateMultiString, UICustomControl),
// UIDialogItem's and Xlator's constructors, HackEnabled, the Vid* queries, strnicmp, __ftol.
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
static Ent g_fns[2048];
static int g_nfns;
struct EntReg { EntReg(const Ent& e) { if (g_nfns < 2048) g_fns[g_nfns++] = e; } };
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

#include "../hook/menu_options.cpp"
#include "../hook/menu_race.cpp"
#include "../hook/menu_leftover.cpp"

using namespace uit;
using namespace mob;

// ---- random numbers --------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x6a09e667u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int irange(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
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
    int32_t detect_calls, detect_after;
    int32_t grab_fail_at;
    int32_t pal_next, alloc_canvas_next;
    int32_t nfiles, file_pos;
    char files[8][24];
    int32_t file_open_mask;
    // this step's
    uint32_t opt_seed;                      // which options are present, and their values
    uint32_t drv_bits;                      // the digital flags
    uint32_t drv_vals[8];                   // the driver's readings (float bits)
    int32_t drv_calls;
    int32_t nmix, mix_default, mix_quality;
    uint32_t mix_volume;
    uint8_t mix_caps[4], mix_null[4];
    int32_t ncars, car_number, car_index, ntracks, track_name_sel;
    uint32_t bench;                         // ReplayBenchmark's result (float bits)
    uint8_t replay_ret, joy_named;
    int32_t opp_count;
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
static uint32_t hash_str(const char* s) { uint32_t h = 2166136261u; if (!s) return 0; for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }

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

// ---- stubs: input and time ------------------------------------------------------------------------------------------------------
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

// ---- stubs: the 2D calls (world_ui.cpp's) ----------------------------------------------------------------------------------------
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

// ---- stubs: text, controls, files (world_ui.cpp's) ------------------------------------------------------------------------------
// an Xlator's text: its key; in the fix build, Main_Menu:DefaultPlayerName's is the round's name (see the header)
static const uint32_t K_DEFAULT_NAME = 0x004f8114;
static const char* g_name_text;
static uint32_t xl_text(uint32_t key) { return MENU_FIXES && key == K_DEFAULT_NAME && g_name_text ? (uint32_t)(uintptr_t)g_name_text : key; }
static void __fastcall stub_xlate(uint32_t* xl_, int) {
    L('XLAT'); L(P(xl_));
    xl_[1] = xl_text(xl_[0]);
    xl_[2] = UI_GU32(S_XLATOR_COOKIE);
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
    for (int i = 0; i < nw && i < 16; i++) { uint32_t v = w[i]; L(on_stack(v) ? 'STAK' : v); }
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

// ---- stubs: this step's callees -----------------------------------------------------------------------------------------------
// the options: a key is present (its value written) or missing (the caller's value logged), by a hash of the section,
// the key and the round
static uint32_t opt_h(const char* sec, const char* key) {
    return (hash_str(readable(sec, 1) ? sec : "") * 31u) ^ hash_str(readable(key, 1) ? key : "") ^ HS->opt_seed;
}
static bool opt_present(uint32_t h) { return (h & 7) != 0; }
static void L_key(const char* sec, const char* key) { LS(sec, 32); LS(key, 64); }
static void __cdecl stub_OptionsGetF(const char* sec, const char* key, uint32_t* v) {
    L('OGF '); L_key(sec, key); L(P(v));
    const uint32_t h = opt_h(sec, key);
    if (!opt_present(h)) { L('MISS'); L(*v); return; }
    static const uint32_t vals[] = {0x00000000u, 0x3f000000u, 0x3f800000u, 0x40000000u, 0xbf800000u, 0x3dcccccdu, 0x7fc00000u, 0x7f800000u};
    *v = vals[(h >> 3) & 7];
}
static void __cdecl stub_OptionsGetI(const char* sec, const char* key, int32_t* v) {
    L('OGI '); L_key(sec, key); L(P(v));
    const uint32_t h = opt_h(sec, key);
    if (!opt_present(h)) { L('MISS'); L((uint32_t)*v); return; }
    *v = (int32_t)((h >> 3) % 7) - 1 + (((h >> 8) & 15) == 0 ? 0x1f : 0);
}
static void __cdecl stub_OptionsGetB(const char* sec, const char* key, uint8_t* v) {
    L('OGB '); L_key(sec, key); L(P(v));
    const uint32_t h = opt_h(sec, key);
    if (!opt_present(h)) { L('MISS'); L(*v); return; }
    *v = (uint8_t)(((h >> 3) & 15) == 0 ? (h >> 7) : (h >> 3) & 1);
}
static void __cdecl stub_OptionsGetS(const char* sec, const char* key, char* buf, int n) {
    L('OGS '); L_key(sec, key); L(P(buf)); L((uint32_t)n);
    const uint32_t h = opt_h(sec, key);
    if (!opt_present(h)) { L('MISS'); LS(buf, 64); return; }
    char t[64];
    sprintf(t, "name%u_%s", (h >> 3) % 1000, (h & 0x100) ? "longer_than" : "p");
    if (n > 0) { strncpy(buf, t, (size_t)n); buf[n - 1] = 0; }
}
static void __cdecl stub_OptionsSetF(const char* sec, const char* key, uint32_t v) { L('OSF '); L_key(sec, key); L(v); }
static void __cdecl stub_OptionsSetI(const char* sec, const char* key, int32_t v) { L('OSI '); L_key(sec, key); L((uint32_t)v); }
static void __cdecl stub_OptionsSetB(const char* sec, const char* key, uint32_t v) { L('OSB '); L_key(sec, key); L(v & 0xff); }
static void __cdecl stub_OptionsSetS(const char* sec, const char* key, const char* v) { L('OSS '); L_key(sec, key); LS(v, 64); }
static void __cdecl stub_OptionsLoadModule(const char* s) { L('OLDM'); LS(s); HS->opt_seed = HS->opt_seed * 2654435761u + 1; }
static void __cdecl stub_OptionsFlush() { L('OFLS'); }
static void __cdecl stub_ControlGet(uint32_t* c, const char* key) {
    L('CGET'); L(P(c)); LS(key, 32);
    const uint32_t h = opt_h("CONTROL", key);
    c[0] = h & 3; c[1] = (h >> 2) & 0xff;
}
static void __cdecl stub_ControlSet(const uint32_t* c, const char* key) { L('CSET'); L(c[0]); L(c[1]); LS(key, 32); }
// the driver
static uint8_t __cdecl stub_DriverBegin() { L('DBEG'); return 1; }
static void __cdecl stub_DriverEnd() { L('DEND'); }
static void __cdecl stub_DriverRefresh() { L('DREF'); }
static void __cdecl stub_DriverUpdate(uint32_t dt) { L('DUPD'); L(dt); }
static float drv_next() { const uint32_t b = HS->drv_vals[HS->drv_calls++ & 7]; return bitsf(b); }
static float __cdecl stub_DriverGetSteering(uint32_t a) { L('DSTR'); L(a); return drv_next(); }
static float __cdecl stub_DriverGetThrottle() { L('DTHR'); return drv_next(); }
static float __cdecl stub_DriverGetBraking() { L('DBRK'); return drv_next(); }
static float __cdecl stub_DriverGetClutch() { L('DCLU'); return drv_next(); }
static uint8_t __cdecl stub_DriverIsSteeringDigital() { L('DDSG'); return (uint8_t)(HS->drv_bits & 1); }
static uint8_t __cdecl stub_DriverIsBrakeDigital() { L('DDBR'); return (uint8_t)((HS->drv_bits >> 1) & 1); }
static uint8_t __cdecl stub_DriverIsThrottleDigital() { L('DDTH'); return (uint8_t)((HS->drv_bits >> 2) & 1); }
// the sound
static void __cdecl stub_SoundRestart() { L('SRST'); }
static void __cdecl stub_SoundToss(const char* s, int32_t n) { L('TOSS'); LS(s); L((uint32_t)n); }
struct FakeMixer { const void* vtbl; int32_t k; };
static FakeMixer g_mixers[4];
static const char* __fastcall fm_GetName(FakeMixer* m, int) { L('MNAM'); L((uint32_t)m->k); static const char* n[] = {"Null", "Soft 8", "Soft 16", "Hardware"}; return n[m->k & 3]; }
static const uint8_t* __fastcall fm_GetCaps(FakeMixer* m, int) { L('MCAP'); L((uint32_t)m->k); return &HS->mix_caps[m->k & 3]; }
static uint8_t __fastcall fm_IsNull(FakeMixer* m, int) { L('MNUL'); L((uint32_t)m->k); return HS->mix_null[m->k & 3]; }
static void* g_mixer_vtbl[3] = {(void*)&fm_GetName, (void*)&fm_GetCaps, (void*)&fm_IsNull};
static int32_t __cdecl stub_MixerCount() { L('MCNT'); return HS->nmix; }
static void* __cdecl stub_MixerGet(int32_t i) { L('MGET'); L((uint32_t)i); return &g_mixers[(uint32_t)i & 3]; }
static void __cdecl stub_MixerSetDefault(int32_t i) { L('MSDF'); L((uint32_t)i); }
static int32_t __cdecl stub_MixerGetDefault() { L('MGDF'); return HS->mix_default; }
static float __cdecl stub_MixerGetVolume() { L('MGVL'); return bitsf(HS->mix_volume); }
static void __cdecl stub_MixerSetVolume(uint32_t v) { L('MSVL'); L(v); }
static void __cdecl stub_MixerSetQuality(int32_t q) { L('MSQL'); L((uint32_t)q); }
static const char* __cdecl stub_MixerGetQualityString(int32_t q) {
    L('MGQS'); L((uint32_t)q);
    static const char* n[] = {"Low", "Medium", "High quality sound", "?"};
    return n[(uint32_t)q < 3 ? q : 3];
}
static int32_t __cdecl stub_MixerGetQuality() { L('MGQL'); return HS->mix_quality; }
// the front end's lists
static const char* k_cars[8] = {"viper", "gts", "rt10", "acr", "jeep", "willys", "cop", "ghost"};
static const char* k_tracks[8] = {"Random", "bemidji", "nfield", "kenyon", "Coliseum", "rotunda", "random x", "temple"};
static int32_t __cdecl stub_GetMaxCarFileNames() { L('GMCF'); return HS->ncars; }
static const char* __cdecl stub_GetCarFileName(int32_t i) { L('GCFN'); L((uint32_t)i); return k_cars[(uint32_t)i & 7]; }
static int32_t __cdecl stub_GetCarFileNumber(const char* s) { L('GCFR'); LS(s); return HS->car_number; }
static int32_t __cdecl stub_HackGetCarIndex() { L('HGCI'); return HS->car_index; }
static int32_t __cdecl stub_GetTrackCount() { L('GTCN'); return HS->ntracks; }
static const char* __cdecl stub_GetTrackName(int32_t i) { L('GTNM'); L((uint32_t)i); return k_tracks[(uint32_t)i & 7]; }
static void __cdecl stub_HackRefresh() { L('HREF'); }
static const char* __cdecl stub_VersionGetBuildString() { L('VERS'); return "v1.0 build 1998"; }
static const char* __cdecl stub_JoyGetName() { L('JOYN'); return HS->joy_named ? "Joystick 2000" : ""; }
static float __cdecl stub_ReplayBenchmark(char* s) { L('BNCH'); LS(s); return bitsf(HS->bench); }
static uint8_t __cdecl stub_ReplayLoadDialog() { L('RPLD'); return HS->replay_ret; }
static void __cdecl stub_PaintKitDo(const char* s, int32_t n) { L('PKIT'); LS(s); L((uint32_t)n); }
// group C's classes: a logging UICustomControl vtable
template <int S> static void __fastcall cc_void(void* self, int) { L('CC  '); L(S); L(P(self)); }
template <int S> static void __fastcall cc_xy(void* self, int, int32_t x, int32_t y) { L('CC  '); L(S); L(P(self)); L((uint32_t)x); L((uint32_t)y); }
static void __fastcall cc_callback(void* self, int, int32_t id, const void* p) { L('CC  '); L(0x10); L(P(self)); L((uint32_t)id); L(P(p)); }
static void __fastcall cc_draw(void* self, int, gxCanvas* c) { L('CC  '); L(0x18); L(P(self)); L(P(c)); }
static void* __fastcall cc_cursor(void* self, int) { L('CC  '); L(0x44); L(P(self)); return fake_for("cc_cursor", 'STMP'); }
static uint8_t __fastcall cc_charhit(void* self, int, uint16_t k) { L('CC  '); L(0x48); L(P(self)); L(k); return (uint8_t)(k & 1); }
static uint8_t __fastcall cc_tabstop(void* self, int) { L('CC  '); L(0x4c); L(P(self)); return 0; }
static void* __fastcall cc_dtor(void* self, int, uint32_t fl) { L('CC  '); L(0); L(P(self)); L(fl); return self; }
static void* g_cc_vtbl[20] = {
    (void*)&cc_dtor, (void*)&cc_void<4>, (void*)&cc_void<8>, (void*)&cc_void<0xc>, (void*)&cc_callback, (void*)&cc_void<0x14>,
    (void*)&cc_draw, (void*)&cc_void<0x1c>, (void*)&cc_xy<0x20>, (void*)&cc_xy<0x24>, (void*)&cc_xy<0x28>, (void*)&cc_xy<0x2c>,
    (void*)&cc_xy<0x30>, (void*)&cc_void<0x34>, (void*)&cc_void<0x38>, (void*)&cc_void<0x3c>, (void*)&cc_void<0x40>,
    (void*)&cc_cursor, (void*)&cc_charhit, (void*)&cc_tabstop,
};
static void fake_control(void* self) { ((uint32_t*)self)[0] = (uint32_t)(uintptr_t)g_cc_vtbl; ((uint32_t*)self)[5] = 0; }
static void* __fastcall stub_TrackChooser_ctor(void* self, int, uint32_t a, uint8_t* b, char* c, void* go) {
    L('TCHC'); L(P(self)); L(a & 0xff); L(P(b)); L(P(c)); L(P(go)); fake_control(self); return self;
}
static void __fastcall stub_TrackChooser_dtor(void* self, int) { L('TCHD'); L(P(self)); }
static void* __fastcall stub_CarChooser_ctor(void* self, int, uint32_t a, char* b, int32_t c, uint32_t d, void* e, void* f) {
    L('CCHC'); L(P(self)); L(a & 0xff); L(P(b)); L((uint32_t)c); L(d & 0xff); L(P(e)); L(P(f)); fake_control(self); return self;
}
static void __fastcall stub_CarChooser_dtor(void* self, int) { L('CCHD'); L(P(self)); }
static void* __fastcall stub_OpponentViewer_ctor(void* self, int, void* go, int32_t* count) {
    L('OPVC'); L(P(self)); L(P(go)); L(P(count)); fake_control(self); *count = HS->opp_count; return self;
}
static void __fastcall stub_OpponentViewer_dtor(void* self, int) { L('OPVD'); L(P(self)); }
static void __fastcall stub_OpponentViewer_SetCar(void* self, int, const char* s) { L('OPSC'); L(P(self)); LS(s); }
static void* __fastcall stub_RaceOptionViewer_ctor(void* self, int, int32_t* a, int32_t* b, void* go, uint8_t* c, char* d) {
    L('ROVC'); L(P(self)); L(P(a)); L(P(b)); L(P(go)); L(P(c)); L(P(d)); fake_control(self); return self;
}
static const char* __fastcall stub_TrackViewer_GetTrackName(void* self, int) { L('TVGN'); L(P(self)); return k_tracks[HS->track_name_sel & 7]; }
static const char* __fastcall stub_CarViewer3D_GetName(void* self, int) { L('CVGN'); L(P(self)); return k_cars[HS->car_index & 7]; }

// ---- the raw call (world_ui.cpp's) --------------------------------------------------------------------------------------------
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
// the stack the call will use, filled with one pattern (so a local the original leaves uninitialised reads the same thing
// in both passes). Called last before the raw call, and written in assembly, top down (the guard pages grow in order): a
// C function's own frame and the calls made after it (_controlfp_s) would leave their return addresses and locals in the
// region the callee's frame uses, at depths the two passes' frames don't share.
__declspec(naked) static void fill_stack() {
    __asm {
        push edi
        push ecx
        push eax
        lea edi, [esp - 4]
        mov ecx, 0xc000
        mov eax, 0xcdcdcdcd
        std
        rep stosd
        cld
        pop eax
        pop ecx
        pop edi
        ret
    }
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
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    _controlfp_s(&cw, g_pc, _MCW_PC);
    fill_stack();
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
static void call_orig(uint32_t fn, int fast, std::initializer_list<uint32_t> words) {
    Ent e = {fn, "(setup)", 0, fast, (int)words.size() - (fast ? 2 : 0), 4, 0, 0};
    uint32_t w[32];
    int i = 0;
    for (uint32_t x : words) w[i++] = x;
    g_pc = _PC_53;
    const Result r = run(e, false, w);
    if (r.fault) printf("setup: %08x faulted (%08x at %08x)\n", fn, r.code, r.eip);
}
#define U32(p) ((uint32_t)(uintptr_t)(p))

struct World {
    gxCanvas* screen;
    WidgetWindow* win;
    GfxOptionsControl* gfx;
    SoundOptionsControl* sound;
    ControlsOptionsControl* controls;
    AidsOptionsControl* aids;
    HackOptionsControl* hack;
    Meter* meter[3];
    float* meter_vals;                              // the bars' values (3)
    int32_t* ints;                                  // MixerChooser's variables, adjust_idle's argument
    char* scratch;                                  // fresh blocks for constructors (0x800)
    char* car_buf;                                  // MenuDoRaceSetup's outputs
    char* track_buf;
    uint8_t* game;                                  // a GameOptions (0x40)
    UIDialogItem* items;
    std::vector<Widget*> widgets;
};
static World W;
static std::vector<uint32_t> g_setup_e;              // the ui library's $E initialisers (setup only)

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
        if (!strcmp(c[2], "ui") && c[5][0] == '$' && c[5][1] == 'E') g_setup_e.push_back((uint32_t)strtoul(c[0], 0, 16));
    }
    fclose(f);
}
static void build_world() {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    HS->nmix = 3; HS->ncars = 5; HS->ntracks = 6; HS->car_number = 0; HS->opt_seed = 0x1234567;
    for (int i = 0; i < 4; i++) { g_mixers[i].vtbl = g_mixer_vtbl; g_mixers[i].k = i; HS->mix_caps[i] = (uint8_t)(i & 1); HS->mix_null[i] = i == 0; }
    UI_G32(S_SCREEN_W) = 640;
    UI_G32(S_SCREEN_H) = 480;
    for (uint32_t a : g_setup_e) call_orig(a, 0, {});
    for (int i = 0; i < g_nfns; i++)
        if (g_fns[i].name[0] == '$') call_orig(g_fns[i].v10, 0, {});
    call_orig(0x004779a0, 0, {});                   // UIBegin
    W.screen = (gxCanvas*)wv(sizeof(gxCanvas));
    W.screen->format = 4; W.screen->pixels = (uint8_t*)0x7b000000; W.screen->w = 640; W.screen->h = 480; W.screen->pitch = 1280;
    W.screen->cx1 = 640; W.screen->cy1 = 480;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    W.ints = (int32_t*)wv(64 * 4);
    W.meter_vals = (float*)wv(16);
    W.scratch = (char*)wv(0x800);
    W.car_buf = (char*)wv(0x100);
    W.track_buf = (char*)wv(0x100);
    W.game = (uint8_t*)wv(0x40);
    // the tabs, by their original constructors
    W.gfx = (GfxOptionsControl*)wv(sizeof(GfxOptionsControl));
    W.sound = (SoundOptionsControl*)wv(sizeof(SoundOptionsControl));
    W.controls = (ControlsOptionsControl*)wv(sizeof(ControlsOptionsControl));
    W.aids = (AidsOptionsControl*)wv(sizeof(AidsOptionsControl));
    W.hack = (HackOptionsControl*)wv(sizeof(HackOptionsControl));
    call_orig(F_Gfx_ctor, 1, {U32(W.gfx), 0});
    call_orig(F_Sound_ctor, 1, {U32(W.sound), 0});
    call_orig(F_Controls_ctor, 1, {U32(W.controls), 0});
    call_orig(F_Aids_ctor, 1, {U32(W.aids), 0});
    call_orig(F_Hack_ctor, 1, {U32(W.hack), 0});
    UI_GP(void, S_GFX_OPTIONS) = W.gfx;
    UI_GP(void, S_CONTROLS_OPTIONS) = W.controls;
    // the bars, as adjust_ctls makes them
    for (int k = 0; k < 3; k++) {
        W.meter[k] = (Meter*)wv(sizeof(Meter));
        W.meter[k]->vtbl = (const void*)(uintptr_t)(k == 2 ? VT_SteerMeter : VT_Meter);
        W.meter[k]->value = &W.meter_vals[k];
    }
    // a full-screen window, running, and the controls added to it by the game's _UIAddItems
    call_orig(F_WidgetCreateWindow, 0, {U32("options.stp"), 0, 0, 640, 480, 2});
    W.win = (WidgetWindow*)(uintptr_t)g_r_eax;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    W.items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 12);
    UICustomControl* ctl[8] = {W.gfx, W.sound, W.controls, W.aids, W.hack, W.meter[0], W.meter[1], W.meter[2]};
    const int32_t pos[8][4] = {{0, 0, 600, 400}, {10, 10, 300, 200}, {20, 20, 400, 300}, {30, 30, 200, 100}, {40, 40, 300, 300},
                               {165, 165, 50, 10}, {165, 265, 50, 10}, {425, 165, 100, 10}};
    for (int k = 0; k < 8; k++) {
        UIDialogItem* it = &W.items[k];
        it->type = 0x17; it->x = pos[k][0]; it->y = pos[k][1]; it->w = pos[k][2]; it->h = pos[k][3]; it->text = "";
        it->data = ctl[k];
    }
    call_orig(F_UIAddItems, 0, {U32(W.win), U32(W.items)});
    for (int i = 0; i < W.win->count; i++) W.widgets.push_back((Widget*)W.win->widgets[i]);
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
static uint32_t fbits_(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float ctl_float() { return chance(15) ? wildf() : range(-1.5f, 1.5f); }
static void randomize_world() {
    for (Widget* w : W.widgets) {
        w->over = chance(40); w->focus = chance(30); w->visible = chance(85); w->enabled = chance(85); w->dirty = chance(50);
    }
    WidgetWindow* w = W.win;
    w->over = chance(50) && w->count ? w->widgets[rnd() % (uint32_t)w->count] : 0;
    w->captured = 0;
    w->focus = chance(60) && w->count ? w->widgets[rnd() % (uint32_t)w->count] : 0;
    w->hidden = chance(30) ? rnd() & 0xff : 0;
    w->disabled = chance(30) ? rnd() & 0xff : 0;
    w->full_redraw = chance(40);
    UI_GP(WidgetWindow, S_ACTIVE) = chance(92) ? W.win : 0;
    UI_G8(S_EXIT) = chance(10);
    UI_GF(S_UI_DT) = chance(10) ? wildf() : range(0.0f, 0.25f);
    UI_G32(S_MOUSE_X) = irange(-10, 650); UI_G32(S_MOUSE_Y) = irange(-10, 490);
    // the controls' values
    GfxOptionsControl* g = W.gfx;
    g->video_mode = irange(0, 5); g->filtering = (uint8_t)(chance(10) ? rnd() : rnd() & 1);
    g->draw_distance = ctl_float(); g->detail_level = ctl_float();
    g->sky = (uint8_t)(rnd() & 1); g->road = (uint8_t)(rnd() & 1); g->car = (uint8_t)(rnd() & 1); g->wall = (uint8_t)(rnd() & 1);
    g->smoke = irange(0, 3); g->shadow = irange(0, 3); g->skids = irange(0, 3); g->fog = (uint8_t)(rnd() & 1);
    g->lighting = (uint8_t)(rnd() & 1); g->specular = irange(0, 3); g->mipmap = (uint8_t)(rnd() & 1); g->mirror = irange(0, 4);
    SoundOptionsControl* s = W.sound;
    s->cd_audio = (uint8_t)(rnd() & 1); s->spotter = (uint8_t)(rnd() & 1);
    s->volume = ctl_float(); s->volume_set = chance(40) ? s->volume : ctl_float(); s->music_volume = ctl_float();
    s->quality = chance(15) ? wildf() : (float)irange(0, 2);
    s->fidelity = chance(20) ? range(0.0f, 1.99f) : (float)irange(0, 1);   // 0..1: see the header
    s->mixer = irange(0, 3); s->restart = (uint8_t)(rnd() & 1);
    s->mixers.cur = chance(50) ? s->mixer : irange(0, 3);
    ControlTestControl* t = &W.controls->test;
    t->steer = ctl_float(); t->throttle = ctl_float(); t->brake = ctl_float(); t->clutch = ctl_float();
    t->timer = chance(10) ? wildf() : range(-0.1f, 0.3f);
    W.controls->force_feedback = (uint8_t)(rnd() & 1); W.controls->game_pad = (uint8_t)(rnd() & 1);
    for (int k = 0; k < 10; k++) { W.controls->ctl[k].a = rnd() & 3; W.controls->ctl[k].b = rnd() & 0xff; }
    W.aids->auto_shifting = (uint8_t)(rnd() & 1); W.aids->auto_clutch = (uint8_t)(rnd() & 1);
    W.aids->traction = irange(0, 3); W.aids->abs = irange(0, 3); W.aids->yaw = irange(0, 3);
    HackOptionsControl* h = W.hack;
    h->show_status = (uint8_t)(rnd() & 1); h->horn_ball = (uint8_t)(rnd() & 1); h->no_walls = (uint8_t)(rnd() & 1); h->pave = (uint8_t)(rnd() & 1);
    h->throttle_boost = ctl_float(); h->grip_boost = ctl_float(); h->gravity = ctl_float(); h->car_index = irange(-1, 6);
    for (int k = 0; k < 3; k++) W.meter_vals[k] = ctl_float();
    for (int k = 0; k < 16; k++) W.ints[k] = irange(-1, 4);
    UI_GP(void, S_CONTROLS_GLOBAL) = chance(90) ? (void*)W.controls : (void*)W.scratch;
    UI_G8(0x004e52e4) = (uint8_t)(chance(50) ? 1 : 0);                   // HackEnabled
    UI_G8(0x004f1e9c) = (uint8_t)(chance(80) ? 1 : 0);                   // VidHas3DHW
    UI_G32(0x00522ad4) = irange(0, 8);                                    // VidGetMegsRam
    for (int k = 0; k < 6; k++) UI_G8(0x00522af8 + k) = (uint8_t)(chance(70) ? 1 : 0);   // the modes supported
    // the stubs' world
    HS->time = (int32_t)(chance(10) ? rnd() : rnd() & 0x7fffff);
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->detect_calls = 0;
    HS->detect_after = irange(1, 8);
    HS->grab_fail_at = chance(20) ? irange(1, 6) : -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->nfiles = irange(0, 6);
    for (int i = 0; i < 8; i++) sprintf(HS->files[i], "track%d.trk", irange(0, 99));
    HS->file_open_mask = (int32_t)rnd();
    HS->script_n = HS->script_pos = 0;
    HS->opt_seed = rnd();
    HS->drv_bits = rnd();
    for (int i = 0; i < 8; i++) HS->drv_vals[i] = fbits_(ctl_float());
    HS->drv_calls = 0;
    HS->nmix = irange(0, 4); HS->mix_default = irange(0, 3); HS->mix_quality = irange(-1, 3);
    HS->mix_volume = fbits_(ctl_float());
    for (int i = 0; i < 4; i++) { HS->mix_caps[i] = (uint8_t)(rnd() & 1); HS->mix_null[i] = (uint8_t)(chance(30) ? 1 : 0); }
    HS->ncars = irange(0, 8); HS->car_number = irange(-1, 7); HS->car_index = irange(0, 7);
    HS->ntracks = chance(3) ? (MENU_FIXES ? irange(3, 9) : 2) : irange(3, 9);   // (the fix build: never two)
#if MENU_FIXES
    static const char* const names[3] = {"Player", "", "Fifteen chars.."};
    g_name_text = names[rnd() % 3];
#endif
    HS->track_name_sel = irange(0, 7);
    HS->bench = fbits_(chance(10) ? wildf() : range(0.0f, 60.0f));
    HS->replay_ret = (uint8_t)(chance(20) ? rnd() : rnd() & 1);
    HS->joy_named = (uint8_t)(rnd() & 1);
    HS->opp_count = irange(0, 8);
}
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x123, 0x124, 0x125, 0x127, 0x12e, 0x126, 0x128, 'q', 'a', 'Z', '5', 'p', 'c', 'o', 5, 1, 0x141};
    const int n = irange(0, maxops);
    for (int i = 0; i < n && i < 62; i++) {
        int32_t* op = HS->script[i];
        const uint32_t r = rnd() % 10;
        op[1] = op[2] = 0;
        if (r < 4) { op[0] = OP_MOVE_W; op[1] = irange(0, 255); op[2] = irange(0, 479); }
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
static bool is_ptr(uint32_t v) {
    return (v >= 0x401000 && v < 0x5d9000) || (v >= U32(g_arena) && v < U32(g_arena) + ARENA_BYTES) || (v >= g_self_lo && v < g_self_hi);
}
static void poison(const Mem& keepfrom) {
    uint32_t* d = (uint32_t*)DATA;
    for (uint32_t i = 0; i < DATA_BYTES / 4; i++) {
        uint32_t v;
        memcpy(&v, keepfrom.data + 4 * i, 4);
        d[i] = is_ptr(v) ? v : rnd();
    }
    static const uint32_t keep[][2] = {
        {0x004f6b78, 0x004f7228}, {0x004f7230, 0x004f72dc},   // ui / widget strings, the menus' first strings
        {0x004f72e4, 0x004f8400},                               // moptions / mrace strings, the race menu's dialog
        {0x004e4db8, 0x004e4dbc},
        {0x004efbf8, 0x004efbfc},                               // the current canvas
        {0x005024b8, 0x005024bc},                               // __adjust_fdiv
        {0x005228d4, 0x005228d8}, {0x005228f4, 0x005228f8},     // the screen's size
        {0x00578d08, 0x00578d40},                               // the end item
        {0x00579730, 0x00579778},                               // the running window, the window table
    };
    for (auto& k : keep) memcpy(DATA + (k[0] - 0x004e1000), keepfrom.data + (k[0] - 0x004e1000), k[1] - k[0]);
}

// the function-local statics (a guard's bit, its Xlator and key): each round, a guard either clear (the function
// constructs its Xlators) or set with the Xlators already constructed (the steady state)
static const struct { uint32_t guard; uint8_t bit; uint32_t xl, key; } k_statics[] = {
    {0x005797cc, 1, 0x00579968, 0x004f79ec},
    {0x00579c08, 1, 0x00579a20, 0x004f7a24}, {0x00579c08, 2, 0x00579bd8, 0x004f7a3c}, {0x00579c08, 4, 0x00579b10, 0x004f7a54},
    {0x00579c08, 8, 0x00579c80, 0x004f7a7c}, {0x00579c08, 0x10, 0x00579c10, 0x004f7a9c}, {0x00579c08, 0x20, 0x00579c58, 0x004f7ab8},
    {0x00579c08, 0x40, 0x00579ac0, 0x004f7ad4}, {0x00579c08, 0x80, 0x00579bf8, 0x004f7af8},
    {0x00579ba0, 1, 0x00579938, 0x004f7b14}, {0x00579ba0, 2, 0x00579a90, 0x004f7b34},
    {0x00579ca4, 1, 0x00579bc8, 0x004f7c98}, {0x00579ca4, 2, 0x00579d48, 0x004f7cc4},
    {0x005d5c4c, 1, 0x005d5c50, 0x004f7e98}, {0x005d5c4c, 2, 0x005d5c60, 0x004f7e84},
    {0x005d5c0c, 1, 0x005d5bf0, 0x004f7f20}, {0x005d5c0c, 2, 0x005d5c00, 0x004f7f08}, {0x005d5c0c, 4, 0x005d5c30, 0x004f7ef0},
    {0x005d5c0c, 8, 0x005d5c40, 0x004f7ed8}, {0x005d5c0c, 0x10, 0x005d5c20, 0x004f7ec4}, {0x005d5c0c, 0x20, 0x005d5c10, 0x004f7eac},
    {0x005d5bac, 1, 0x005d5bb0, 0x004f80ac}, {0x005d5bac, 2, 0x005d5bd0, 0x004f8094}, {0x005d5bac, 4, 0x005d5be0, 0x004f8078},
    {0x005d5bac, 8, 0x005d5bc0, 0x004f8064},
    {0x00579eb0, 1, 0x00579e20, 0x004f8114}, {0x00579eb0, 2, 0x00579d90, 0x004f8130}, {0x00579eb0, 4, 0x00579eb8, 0x004f8148},
    {0x00579eb0, 8, 0x00579df8, 0x004f8160}, {0x00579eb0, 0x10, 0x00579e40, 0x004f8174}, {0x00579eb0, 0x20, 0x00579f00, 0x004f818c},
    {0x00579eb0, 0x40, 0x00579e10, 0x004f81a0}, {0x00579eb0, 0x80, 0x00579d68, 0x004f81b4},
    {0x00579d88, 1, 0x00579d78, 0x004f82d8}, {0x00579d88, 2, 0x00579ec8, 0x004f82ec}, {0x00579d88, 4, 0x00579e30, 0x004f82fc},
    {0x00579d88, 8, 0x00579ea0, 0x004f830c}, {0x00579d88, 0x10, 0x00579e90, 0x004f831c}, {0x00579d88, 0x20, 0x00579ed8, 0x004f832c},
};
static void randomize_statics() {
    uint32_t last = 0;
    bool constructed = false;
    for (const auto& s : k_statics) {
        if (s.guard != last) {                               // one choice per guard
            last = s.guard;
            constructed = chance(50);
            UI_G8(s.guard) = 0;
        }
        if (constructed) {
            UI_G8(s.guard) = (uint8_t)(UI_G8(s.guard) | s.bit);
            UI_GU32(s.xl) = s.key;
            const bool fresh = chance(50);                    // translated since the last language change, or stale
            UI_GU32(s.xl + 4) = fresh ? xl_text(s.key) : 0;
            UI_GU32(s.xl + 8) = fresh ? UI_GU32(S_XLATOR_COOKIE) : ~UI_GU32(S_XLATOR_COOKIE);
        }
    }
    // the race menu's dialog: its list set once (to the first call's frame): mostly not yet
    const bool set = chance(10);
    UI_G8(S_RACEMENU_DLG_ONCE) = (uint8_t)(set ? 1 : 0);
    UI_GU32(S_RACEMENU_DLG + 0xc) = set ? U32(&W.items[8]) : 0;          // (an empty list: the world's spare items)
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static void fill_random(void* p, uint32_t n) { uint8_t* b = (uint8_t*)p; for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)rnd(); }
static uint32_t canvas_arg() {
    if (chance(60) && W.win) return U32(&W.win->canvas);
    return U32(W.screen);
}
static void* self_for(const char* cls) {
    if (!strcmp(cls, "GfxOptionsControl")) return W.gfx;
    if (!strcmp(cls, "SoundOptionsControl")) return W.sound;
    if (!strcmp(cls, "ControlsOptionsControl")) return W.controls;
    if (!strcmp(cls, "AidsOptionsControl")) return W.aids;
    if (!strcmp(cls, "HackOptionsControl")) return W.hack;
    if (!strcmp(cls, "ControlTestControl")) return &W.controls->test;
    if (!strcmp(cls, "MixerChooser")) return &W.sound->mixers;
    if (!strcmp(cls, "Meter")) return W.meter[irange(0, 1)];
    if (!strcmp(cls, "SteerMeter")) return W.meter[2];
    return 0;
}
// the arguments for f (words: ecx, edx first for a __thiscall); false: skip this round
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const char* nm = f.name;
    if (nm[0] == '$' || strstr(nm, "destructor helper")) return true;
    char cls[64] = "";
    const char* meth = nm;
    if (const char* sep = strstr(nm, "::")) {
        const size_t n = (size_t)(sep - nm) < 63 ? (size_t)(sep - nm) : 63;
        memcpy(cls, nm, n);
        cls[n] = 0;
        meth = sep + 2;
    }
    uint32_t* a = f.fast ? w + 2 : w;
    fill_random(W.scratch, 0x800);
    if (cls[0]) {
        if (!strcmp(meth, cls)) {                            // a constructor: a fresh random block
            w[0] = U32(W.scratch);
            if (!strcmp(cls, "MixerChooser")) { a[0] = U32(&W.ints[irange(0, 3)]); a[1] = U32(&W.ints[irange(4, 7)]); a[2] = U32(&W.ints[irange(8, 11)]); }
            return true;
        }
        if (!strcmp(meth, "default_ctl")) { a[0] = (uint32_t)(chance(92) ? irange(0, 2) : irange(-2, 5)); return true; }
        void* self = self_for(cls);
        if (!self) return false;
        w[0] = U32(self);
        if (strstr(meth, "deleting destructor")) a[0] = rnd() & 3;
        else if (!strcmp(meth, "Draw")) a[0] = canvas_arg();
        else if (!strcmp(meth, "Callback")) { a[0] = (uint32_t)(chance(85) ? irange(0, 2) : irange(-1, 5)); a[1] = chance(50) ? 0 : U32(&W.ints[0]); }
        return true;
    }
#define IS(n) (!strcmp(nm, n))
    if (IS("MenuDoRaceSetup")) {
        fill_random(W.car_buf, 0x100); fill_random(W.track_buf, 0x100); fill_random(W.game, 0x40);
        a[0] = U32(W.car_buf); a[1] = U32(W.track_buf); a[2] = U32(W.game);
    } else if (IS("adjust_idle")) a[0] = U32(&W.ints[irange(0, 15)]);
    else if (IS("DoFlyout")) { a[0] = U32("main.stp"); a[1] = rnd() & 0xff; }
    else a[0] = rnd();                                          // run_benchmark, adjust_ctls, tune_default, the callbacks
#undef IS
    return true;
}

// VP_TAGS=1 (with VP_ONLY): how often each stub was reached in the originals' passes, and how the calls returned -- to see
// which paths the rounds cover
struct Tag { uint32_t tag; int n; };
static Tag g_tags[256];
static int g_ntags;
static int g_ret_hist[8];
static void tag_count(const CallLog& lg, const Result& r) {
    for (uint32_t i = 0; i < lg.n && i < LOG_MAX; i++) {
        const uint32_t w = lg.w[i];
        bool tag = true;
        for (int b = 0; b < 4; b++) { const uint8_t c = (uint8_t)(w >> (8 * b)); if (!(c == ' ' || (c >= 'A' && c <= 'Z'))) tag = false; }
        if (!tag) continue;
        int k = 0;
        while (k < g_ntags && g_tags[k].tag != w) k++;
        if (k == g_ntags && g_ntags < 256) g_tags[g_ntags++] = {w, 0};
        if (k < 256) g_tags[k].n++;
    }
    g_ret_hist[r.fault ? 7 : r.ret == 0xfffffffeu ? 0 : r.ret == 0xffffffffu ? 1 : r.ret == 0 ? 2 : r.ret == 1 ? 3 : r.ret == 0x77 ? 4 : 5]++;
}
static void tag_print() {
    printf("stubs reached (original passes):");
    for (int k = 0; k < g_ntags; k++) printf(" %c%c%c%c %d", (char)(g_tags[k].tag >> 24), (char)(g_tags[k].tag >> 16), (char)(g_tags[k].tag >> 8), (char)g_tags[k].tag, g_tags[k].n);
    printf("\nreturns: -2 %d, -1 %d, 0 %d, 1 %d, 0x77 (the watchdog) %d, other %d, faulted %d\n", g_ret_hist[0], g_ret_hist[1], g_ret_hist[2],
           g_ret_hist[3], g_ret_hist[4], g_ret_hist[5], g_ret_hist[7]);
}

#if MENU_FIXES
// ---- the fixes, each on its bad case (directed_fix_tests) -----------------------------------------------------------------
// The rewrite alone on the case the original gets wrong, from a round's world (fixed seeds, not poisoned): it must return
// cleanly (no fault, the bytes popped, ebx / esi / edi / ebp kept) and give what the fix promises, checked against the
// original run on the input the fix makes of the bad one: every byte of .data/.bss/.idata and the arena, the return and
// the call logs.
static int g_fx_bad, g_fx_n;
static const Ent& fx_fn(const char* name) {
    for (int i = 0; i < g_nfns; i++)
        if (!strcmp(g_fns[i].name, name)) return g_fns[i];
    printf("  fix test: %s isn't listed\n", name);
    fflush(stdout);
    ExitProcess(4);
}
static Result fx_run(const Ent& f, bool rewrite, const uint32_t* w) {
    uint32_t words[72] = {};
    memcpy(words, w, 4 * (size_t)(f.nstack + (f.fast ? 2 : 0)));
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
    if (r) printf(" (fault %d %08x at %08x, return %08x, popped %u, ebx esi edi ebp %08x %08x %08x %08x)", r->fault, r->code, r->eip,
                  r->ret, r->pops, r->regs[0], r->regs[1], r->regs[2], r->regs[3]);
    if (where) printf(" (first differs at %08x)", where);
    printf("\n");
}
static CallLog g_fx_log;
static bool fx_log_same() { return g_log.n == g_fx_log.n && !memcmp(g_log.w, g_fx_log.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)); }
static void fx_keep_log() { memcpy(&g_fx_log, &g_log, sizeof(uint32_t) * (1 + (g_log.n < LOG_MAX ? g_log.n : LOG_MAX))); }
static bool fx_logged(uint32_t tag, uint32_t next) {
    for (uint32_t i = 0; i + 1 < g_log.n && i + 1 < LOG_MAX; i++)
        if (g_log.w[i] == tag && g_log.w[i + 1] == next) return true;
    return false;
}
// a round's world from a fixed seed (as the main loop makes one, not poisoned)
static void fx_world(uint32_t seed, int ops) {
    g_rng = seed * 2654435761u | 1;
    mem_load(g_pristine);
    randomize_world();
    randomize_statics();
    random_script(ops);
}
// the stubbed option `key` of `sec` present or missing (OptionsGet* finds it or not by a hash of the seed)
static void fx_option(const char* sec, const char* key, bool present) {
    while (opt_present(opt_h(sec, key)) != present) HS->opt_seed++;
}

static int directed_fix_tests() {
    char m[200];
    // ---- 4. MenuDoRaceSetup: a random track with two tracks ----
    {
        const Ent& f = fx_fn("MenuDoRaceSetup");
        int orig_faults = 0, cases = 0;
        for (int k = 0; k < 16; k++) {
            fx_world(0x4000 + k, 0);
            HS->script[0][0] = OP_KEY; HS->script[0][1] = 0xd; HS->script[0][2] = 0;   // Enter: the default button, -2 (race)
            HS->script_n = 1; HS->script_pos = 0;
            HS->ntracks = 2;
            HS->track_name_sel = (k & 1) ? 0 : 6;                                       // "Random", "random x": both random
            HS->time = k < 4 ? 16 * k : (int32_t)(rnd() & 0x7fffffff);
            memset(W.car_buf, 0xa5, 0x100); memset(W.track_buf, 0xa5, 0x100); fill_random(W.game, 0x40);
            const uint32_t w[3] = {U32(W.car_buf), U32(W.track_buf), U32(W.game)};
            mem_save(g_snap);
            const Result ro2 = fx_run(f, false, w);                                     // the original: divides by zero
            orig_faults += ro2.fault && ro2.code == EXCEPTION_INT_DIVIDE_BY_ZERO;
            mem_load(g_snap);
            const Result rn = fx_run(f, true, w);
            cases++;
            sprintf(m, "MenuDoRaceSetup, two tracks (case %d): a clean return", k);
            fx_check(fx_clean(f, rn), m, &rn);
            sprintf(m, "MenuDoRaceSetup, two tracks (case %d): the race chosen, GetTrackName(0), row 0's name out, nothing past it", k);
            fx_check(rn.ret == 1 && fx_logged('GTNM', 0) && !strcmp(W.track_buf, k_tracks[0]) &&
                         (uint8_t)W.track_buf[strlen(k_tracks[0]) + 1] == 0xa5, m, &rn);
            mem_save(g_after);
            fx_keep_log();
            mem_load(g_snap);
            HS->ntracks = 1;                                                            // the original with one: t % -1 == 0
            const Result ro1 = fx_run(f, false, w);
            HS->ntracks = 2;
            const uint32_t where = mem_diff(g_after);
            sprintf(m, "MenuDoRaceSetup, two tracks (case %d): what the original does with one track", k);
            fx_check(!ro1.fault && ro1.ret == rn.ret && where == 0 && fx_log_same(), m, &ro1, where);
        }
        sprintf(m, "MenuDoRaceSetup, two tracks: the original faults dividing by zero (%d of %d)", orig_faults, cases);
        fx_check(orig_faults == cases, m);
    }
    // ---- 5. MenuDoRaceMenu: a default name over 15 characters ----
    {
        const Ent& f = fx_fn("MenuDoRaceMenu");
        static char text[0x1000], cut[16];
        for (int i = 0; i < 0xfff; i++) text[i] = (char)('A' + i % 26);
        static const int lens[] = {16, 17, 27, 40, 300, 0x437, 0xfff};
        int orig_differs = 0, orig_cases = 0;
        for (int li = 0; li < (int)(sizeof lens / sizeof lens[0]); li++)
            for (int k = 0; k < 6; k++) {
                const int len = lens[li];
                const bool present = k & 1;
                fx_world(0x5000 + 16 * li + k, 16);
                const char saved = text[len];
                text[len] = 0;
                memcpy(cut, text, 15); cut[15] = 0;
                fx_option(UI_GP(const char, S_SEC_RACE_GLOBAL), (const char*)0x004f81c4, present);   // player_name
                // the Xlator made and fresh, its text the long name
                UI_G8(S_RACEMENU_ONCE) = (uint8_t)(UI_G8(S_RACEMENU_ONCE) | 1);
                UI_GU32(0x00579e20) = K_DEFAULT_NAME;
                UI_GU32(0x00579e24) = U32(text);
                UI_GU32(0x00579e28) = UI_GU32(S_XLATOR_COOKIE);
                g_name_text = text;
                const uint32_t w[1] = {0};
                mem_save(g_snap);
                if (len == 27 || len == 40) {                                           // the original on the long name: wrong
                    const Result rb = fx_run(f, false, w);
                    fx_keep_log();
                    mem_load(g_snap);
                    fx_run(f, true, w);
                    orig_cases++;
                    orig_differs += rb.fault || !fx_log_same();
                    mem_load(g_snap);
                }
                const Result rn = fx_run(f, true, w);
                sprintf(m, "MenuDoRaceMenu, a %d-character name (%s saved name, case %d): a clean return", len, present ? "a" : "no", k);
                fx_check(fx_clean(f, rn), m, &rn);
                mem_save(g_after);
                fx_keep_log();
                mem_load(g_snap);
                UI_GU32(0x00579e24) = U32(cut);                                         // the original on its first 15
                g_name_text = cut;
                const Result ro = fx_run(f, false, w);
                UI_GU32(0x00579e24) = U32(text);
                const uint32_t where = mem_diff(g_after);
                sprintf(m, "MenuDoRaceMenu, a %d-character name (%s saved name, case %d): the original on its first 15 characters",
                        len, present ? "a" : "no", k);
                fx_check(!ro.fault && ro.ret == rn.ret && where == 0 && fx_log_same(), m, &ro, where);
                text[len] = saved;
                g_name_text = 0;
            }
        printf("  (MenuDoRaceMenu: the original on the long name itself went wrong -- faulted, or logged other bytes -- in %d of %d "
               "cases of 27 and 40 characters)\n", orig_differs, orig_cases);
    }
    printf("directed fix tests: %s -- %d checks, %d failed\n", g_fx_bad ? "FAILED" : "all passed", g_fx_n, g_fx_bad);
    return g_fx_bad;
}
#endif

// ---- main -----------------------------------------------------------------------------------------------------------------------
static bool is_modal(const char* nm) {
    return strstr(nm, "MenuDo") == nm || !strcmp(nm, "adjust_ctls") || !strcmp(nm, "tune_default") || strstr(nm, "::Added");
}
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    const int rounds = argc > 1 ? atoi(argv[1]) : 24;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    const char* only = getenv("VP_ONLY");
    const bool tags = GetEnvironmentVariableA("VP_TAGS", 0, 0) != 0;
    char exe[MAX_PATH], inv[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_menu_options.cpp");
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
        // this step's
        {F_OptionsGetF, (void*)&stub_OptionsGetF}, {F_OptionsGetI, (void*)&stub_OptionsGetI}, {F_OptionsGetB, (void*)&stub_OptionsGetB},
        {F_OptionsGetS, (void*)&stub_OptionsGetS}, {F_OptionsSetF, (void*)&stub_OptionsSetF}, {F_OptionsSetI, (void*)&stub_OptionsSetI},
        {F_OptionsSetB, (void*)&stub_OptionsSetB}, {F_OptionsSetS, (void*)&stub_OptionsSetS},
        {F_OptionsLoadModule, (void*)&stub_OptionsLoadModule}, {F_OptionsFlush, (void*)&stub_OptionsFlush},
        {F_ControlGet, (void*)&stub_ControlGet}, {F_ControlSet, (void*)&stub_ControlSet},
        {F_DriverBegin, (void*)&stub_DriverBegin}, {F_DriverEnd, (void*)&stub_DriverEnd}, {F_DriverRefresh, (void*)&stub_DriverRefresh},
        {F_DriverUpdate, (void*)&stub_DriverUpdate}, {F_DriverGetSteering, (void*)&stub_DriverGetSteering},
        {F_DriverGetThrottle, (void*)&stub_DriverGetThrottle}, {F_DriverGetBraking, (void*)&stub_DriverGetBraking},
        {F_DriverGetClutch, (void*)&stub_DriverGetClutch}, {F_DriverIsSteeringDigital, (void*)&stub_DriverIsSteeringDigital},
        {F_DriverIsBrakeDigital, (void*)&stub_DriverIsBrakeDigital}, {F_DriverIsThrottleDigital, (void*)&stub_DriverIsThrottleDigital},
        {F_SoundRestart, (void*)&stub_SoundRestart}, {F_SoundToss, (void*)&stub_SoundToss}, {F_MixerCount, (void*)&stub_MixerCount},
        {F_MixerGet, (void*)&stub_MixerGet}, {F_MixerSetDefault, (void*)&stub_MixerSetDefault}, {F_MixerGetDefault, (void*)&stub_MixerGetDefault},
        {F_MixerGetVolume, (void*)&stub_MixerGetVolume}, {F_MixerSetVolume, (void*)&stub_MixerSetVolume},
        {F_MixerSetQuality, (void*)&stub_MixerSetQuality}, {F_MixerGetQualityString, (void*)&stub_MixerGetQualityString},
        {F_MixerGetQuality, (void*)&stub_MixerGetQuality},
        {F_GetMaxCarFileNames, (void*)&stub_GetMaxCarFileNames}, {F_GetCarFileName, (void*)&stub_GetCarFileName},
        {F_GetCarFileNumber, (void*)&stub_GetCarFileNumber}, {F_HackGetCarIndex, (void*)&stub_HackGetCarIndex},
        {F_GetTrackCount, (void*)&stub_GetTrackCount}, {F_GetTrackName, (void*)&stub_GetTrackName}, {F_HackRefresh, (void*)&stub_HackRefresh},
        {F_VersionGetBuildString, (void*)&stub_VersionGetBuildString}, {F_JoyGetName, (void*)&stub_JoyGetName},
        {F_ReplayBenchmark, (void*)&stub_ReplayBenchmark}, {F_ReplayLoadDialog, (void*)&stub_ReplayLoadDialog},
        {F_PaintKitDo, (void*)&stub_PaintKitDo},
        {F_TrackChooser_ctor, (void*)&stub_TrackChooser_ctor}, {F_TrackChooser_dtor, (void*)&stub_TrackChooser_dtor},
        {F_CarChooser_ctor, (void*)&stub_CarChooser_ctor}, {F_CarChooser_dtor, (void*)&stub_CarChooser_dtor},
        {F_OpponentViewer_ctor, (void*)&stub_OpponentViewer_ctor}, {F_OpponentViewer_dtor, (void*)&stub_OpponentViewer_dtor},
        {F_OpponentViewer_SetCar, (void*)&stub_OpponentViewer_SetCar}, {F_RaceOptionViewer_ctor, (void*)&stub_RaceOptionViewer_ctor},
        {F_TrackViewer_GetTrackName, (void*)&stub_TrackViewer_GetTrackName}, {F_CarViewer3D_GetName, (void*)&stub_CarViewer3D_GetName},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    build_world();
    mem_save(g_pristine);
    printf("world: %d widgets in the running window, %u KB of heap used; %d ui initialisers run at setup\n", (int)W.widgets.size(),
           (HS->heap_next - A_HEAP) / 1024, (int)g_setup_e.size());

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
    long long checks = 0, poisoned_checks = 0, log_words = 0;
    int differ = 0, fp_bad = 0, faults = 0, pure_n = 0, replay_n = 0, changed_fns = 0, still_fns = 0, bad_fns = 0, skipped = 0;
    int n_e = 0;
    long long both_fault = 0;
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        n_e += f.name[0] == '$';
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0;
        const int nr = is_modal(f.name) || f.name[0] == '$' ? rounds : rounds * 10;
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            randomize_world();
            if (poisoned) poison(g_pristine);
            randomize_statics();
            random_script(is_modal(f.name) ? 16 : 4);
            uint32_t words[72];
            if (!make_args(f, words)) { skipped++; continue; }
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            mem_save(g_snap);
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
                    if (g_snap.arena[i2] != g_arena[i2] && (fp.pure || !covered(g_arena + i2))) out = U32(g_arena + i2);
                if (out) {
                    printf("  FOOTPRINT %08x %s: the original changed %08x outside it (round %d)\n", f.v10, f.name, out, rd);
                    fp_bad++;
                    fn_bad = true;
                }
            }
            mem_save(g_after);
            memcpy(&g_log_orig, &g_log, sizeof(uint32_t) * (1 + (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)));
            if (tags) tag_count(g_log_orig, ro);
            mem_load(g_snap);
            const Result rn = run(f, true, words);
            checks++;
            fn_checks++;
            poisoned_checks += poisoned;
            log_words += g_log_orig.n;
            if (ro.fault || rn.fault) {
                faults++;
                fn_faults++;
                if (trace && fn_faults <= 3) printf("    round %d (%s) faulted: %08x at %08x / %08x at %08x\n", rd, poisoned ? "poisoned" : "as loaded", ro.code, ro.eip, rn.code, rn.eip);
            }
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
                    else if (a2 >= U32(g_arena) && a2 < U32(g_arena) + ARENA_BYTES) { memcpy(&o, g_after.arena + (a2 - U32(g_arena)), 4); memcpy(&n2, (void*)(uintptr_t)a2, 4); }
                    printf("    memory first differs at %08x (arena +%x): %08x / %08x\n", where, where - U32(g_arena), o, n2);
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
    if (tags) tag_print();
    printf("%d functions (%d static initialisers; %d pure, %d replay_only, %d others), %d listed twice; %lld checks (%lld on poisoned .data), "
           "%lld logged words compared: %d differ, %d footprint violations, %d checks faulted (%lld in both, the same way), %d skipped; "
           "%d functions bad\n", g_nfns, n_e, pure_n, replay_n, g_nfns - pure_n - replay_n, dup, checks, poisoned_checks, log_words, differ,
           fp_bad, faults, both_fault, skipped, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    return differ || fp_bad || dup || fix_bad ? 1 : 0;
}
