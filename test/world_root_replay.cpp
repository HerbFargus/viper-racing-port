// world_root_replay.cpp -- M3 UI stage, step U3 (group C): every rewrite of hook/root_replay.cpp (replay.obj) and
// hook/root_ghost.cpp (ghost.obj) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_root_replay.cpp
//        /Fo<dir>\ /Fe<dir>\world_root_replay.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_root_replay.exe [rounds] [seed]      (VP_TRACE=1: one line per function; VP_ONLY=text: those only;
//          VP_DEBUG_WORLD=1: every fault's registers)
//
// Built as test/world_menu_view.cpp is (its loader, raw call, memory comparison and stubs are copied here; the stack is
// filled after the _controlfp_s calls, as test/world_menu_options.cpp does): out\race_v10.exe loaded at 0x400000 in a
// child process with the range reserved, the two rewrite files included with PORT_FN redefined to list each function
// (its v1.0 address, the rewrite, its calling convention, stack arguments, return, footprint). VP_FAITHFUL (the files
// have no fixes).
//
// The world. Once: the $E static initialisers of library `ui` and of replay.obj and ghost.obj, the game's UIBegin; a
// full-screen window made the running one; in it, by the game's _UIAddItems, a ReplayView (its JogControl built by the
// original constructor; its Added -- the original -- fills the window with the replay screen's buttons, hot keys and
// the jog wheel's CustomWidget) and a GraphControl, then their Creates; a game World (the car list, the options), a
// CarMessage for every car manager slot, three LiveGhosts, the best and the submission one, a fake file's bytes, a
// ghost car object, buffers. This is the pristine state.
//
// Then for EVERY function, `rounds` times (four times that for one that runs no dialog or loop): pristine restored;
// half the rounds POISONED (.data/.bss random but for the strings and tables the code reads and what the ui library
// follows); the world random: the replay flags (a replay loaded, from the race, auto-load), the shuttle, camera, graph
// (type -1..6, zoom and scale sometimes NaN / infinite / huge), frame counters, the replay file's and directory's names,
// every function-local Xlator of both files built or not (a built one's cookie fresh or stale), the car list (1..8
// cars, player / AI / network / ghost subtypes, a repeated car sometimes), the options (event, ghost type, field, race
// type, mirror), the ghost statics (target, incognito, the slots -- some 0x7fffffff --, the three LiveGhosts' flags,
// best, car and header, the current / best / submission pointers, sometimes none), the car managers' human flags,
// names and velocities, the physics tick, the fake file (a replay World or a ghost file: version, size, realism, track,
// its packets' positions sometimes NaN / infinite / far apart; its size, whether it opens, reads, loads); the stubs'
// answers (paused, at end, the scan keys, the file boxes, the telemetry, the network, feed_ghost_car). Arguments made
// for each function; for those that run a dialog or loop a random input script, ended by a watchdog (after 40 frames
// the dialog's exit flag, the full screen gets a click, the benchmark's replay pauses). Snapshot; the stack filled; the
// ORIGINAL runs from a raw call thunk; its results kept, the snapshot restored, the REWRITE runs. Compared:
// .data/.bss/.idata and the 3 MB arena (a pointer into this thread's stack in both counts as equal), the return value
// (st0 for convert_ticks_to_time / ReplayBenchmark), the x87 depth, bytes popped, ebx / esi / edi / ebp, faults, every
// stub's call log. Every byte the original changed must lie in the rewrite's footprint (unless replay_only); a pure one
// changes nothing. The x87 at 24 or 53 bits (alternating rounds).
//
// Stubbed (logged; state in the arena so both passes see the same): world_menu_view.cpp's set (memory, logs, input
// and time, the 2D calls, Xlator::xlate, sprintf, files) and this group's callees outside replay.obj / ghost.obj: the
// replay player (PhysReplay*), the world (WorldSwitchTo*, WorldBegin/EndReplay, WorldUpdate / Draw, the focus car --
// CameraSetFocusCar's static written as the game's does --, WorldGameOptions / GetTrackname / GetCarEntry, FX),
// telemetry, sound, resources, the renderer (mrSetView, mrBeginFrame / EndFrame, mrModelFlush), the cars (LoadCar(s),
// UnloadCar(s)), SplashLoading, move_blimp, GetCameraName, GetTrackNumber, PhysicsTimeString, CameraSetType, ScanDown,
// MousePeekEvent, the file boxes (UIDoOpenFileBox / SaveFileBox / OkBox), every file call (FileOpen / Create / Size /
// ReadExact / Write / Close / SetWritable / Remove / CreateDirectory: a fake file in the arena; a write logs its bytes'
// hash), MemFree, the tasks and profiler, the network (MultiEnabled, MultiCarIsHuman), AIGetMaxGhostSubmitTicks,
// feed_ghost_car, GhostCar::NewLap, Win32GetUserDirectory. The game's own code everywhere else: the widget toolkit
// (UIDoDialog, _UIAddItems, the groups, UIDeltaT, _UIUpdateTime), UICustomControl's AddNotification / Dirty /
// AddItems, Xlator's constructor, CarMgrGetInfo, World::is_valid_version, PhysicsGetTime, the CRT (strncpy, strrchr,
// memmove, _CIfmod, _isnan, _finite, __ftol), and every function of this group (each rewrite is checked against its
// original with the same callees).
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

#define VP_FAITHFUL
#include "../hook/port.h"

// ---- the registry: PORT_FN lists each function -------------------------------------------------------------------------
struct Ent {
    uint32_t v10;
    const char* name;
    void* fn;
    int fast;
    int nstack;
    int ret;
    int retf;
    void (*fp)(Footprint&, const uint32_t*);
};
static Ent g_fns[256];
static int g_nfns;
struct EntReg { EntReg(const Ent& e) { if (g_nfns < 256) g_fns[g_nfns++] = e; } };
template <typename T> static constexpr int words_of() { return (int)((sizeof(T) + 3) / 4); }
template <typename T> static T from_words(const uint32_t* w) { T t; memcpy(&t, w, sizeof(T)); return t; }
template <typename R> static constexpr int ret_width() { if constexpr (std::is_void_v<R> || std::is_floating_point_v<R>) return 0; else return (int)sizeof(R); }
template <typename... A> struct Offs {
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

#include "../hook/root_replay.cpp"
#include "../hook/root_ghost.cpp"

using namespace uit;
using namespace rrep;

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
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* const IDATA = (uint8_t*)0x005d7000;
enum { IDATA_BYTES = 0x136a };
enum { ARENA_BYTES = 0x300000 };
static uint8_t* g_arena;
struct Mem { uint8_t* data; uint8_t* idata; uint8_t* arena; };
static Mem g_pristine, g_snap, g_after;
static Mem mem_alloc() { return {(uint8_t*)malloc(DATA_BYTES), (uint8_t*)malloc(IDATA_BYTES), (uint8_t*)malloc(ARENA_BYTES)}; }
static void mem_save(Mem& m) { memcpy(m.data, DATA, DATA_BYTES); memcpy(m.idata, IDATA, IDATA_BYTES); memcpy(m.arena, g_arena, ARENA_BYTES); }
static void mem_load(const Mem& m) { memcpy(DATA, m.data, DATA_BYTES); memcpy(IDATA, m.idata, IDATA_BYTES); memcpy(g_arena, m.arena, ARENA_BYTES); }
static ULONG_PTR g_stack_lo, g_stack_hi;
static bool on_stack(uint32_t a) { return a >= g_stack_lo && a < g_stack_hi; }
// the next index >= i where a and b differ (n if none), skipping equal 4 KB blocks with memcmp
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
    // this group's stubs
    uint32_t paused_bits, paused_n, atend_bits, atend_n, scan_bits, scan_n, tel_bits, tel_n;
    float tel[5];
    int32_t file_ok, file_size, read_ok, fpos, flen, create_ok, box_ok, rload_ok, nopen;
    int32_t focus, player, multi, human_mask, maxsubmit, feed_n, feed_ret;
};
#define HS ((HState*)g_arena)
enum : uint32_t { A_HEAP = 0x100000 };

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
static uint32_t hash_str(const char* s) {
    uint32_t h = 2166136261u;
    if (!s || !readable(s, 1)) return 0x5eed;
    for (; readable(s, 1) && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static uint32_t hash_mem(const void* p, uint32_t n) {
    uint32_t h = 2166136261u;
    const uint8_t* b = (const uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// ---- stubs: memory, logs, sync --------------------------------------------------------------------------------------------------
static void* __cdecl stub_MemAlloc(int n) {
    L('ALOC'); L((uint32_t)n);
    uint32_t a = (HS->heap_next + 7) & ~7u;
    if (a < A_HEAP || n < 0 || n > 0x200000 || a + (uint32_t)n > ARENA_BYTES - 0x100) { L('OOM '); return 0; }
    HS->heap_next = a + (uint32_t)n;
    return g_arena + a;
}
static void __cdecl stub_delete(void* p) { L('DEL '); L(P(p)); }
static void __cdecl stub_MemFree(void* p) { L('FREE'); L(P(p)); }
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
static bool script_mouse_next() { return HS->script_pos < HS->script_n && HS->script[HS->script_pos][0] != OP_KEY; }
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
    if (HS->frames > HS->frame_limit) {                       // the watchdog: a click ends the full screen
        ev->type = 0; ev->x = HS->mouse_x; ev->y = HS->mouse_y; ev->state = 0;
        L('WCLK');
        return 1;
    }
    return 0;
}
static uint8_t __cdecl stub_MousePeekEvent() {
    L('MPEV');
    return script_mouse_next() || (HS->script_pos >= HS->script_n && HS->frames > HS->frame_limit) ? 1 : 0;
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
static uint8_t __cdecl stub_ScanDown(uint32_t k) {
    L('SCAN'); L(k & 0xff);
    return (uint8_t)((HS->scan_bits >> (HS->scan_n++ & 31)) & 1);
}

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
static void __cdecl stub_gxFlip() { L('FLIP'); HS->frames++; }
static void __cdecl stub_mrBeginFrame() { L('MRBF'); }
static void __cdecl stub_mrEndFrame() { L('MREF'); }

// ---- stubs: text, controls, files --------------------------------------------------------------------------------------------------
static const char* const k_xl_text[16] = {"s", "bhp", "rpm", "lb-ft", "mph", "Laps", "Pack", "Ghost", "Clock", "DNF", "Best",
                                          "Car", "Track", "Weather", "Time of day", "A longer translation"};
static uint32_t g_xl_seed;
static const char* xl_text(uint32_t key) {
    const uint32_t h = hash_str((const char*)(uintptr_t)key) ^ g_xl_seed;
    return k_xl_text[(h & 0xff) < 0xf0 ? h % 14 : 14 + (h & 1)];
}
static void __fastcall stub_xlate(uint32_t* xl, int) {
    L('XLAT'); L(P(xl));
    xl[1] = (uint32_t)(uintptr_t)xl_text(xl[0]);
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
        if (*p == 's') { LS((const char*)(uintptr_t)w[nw]); }
        nw += strchr("eEfgG", *p) ? 2 : 1;
    }
    for (int i = 0; i < nw && i < 16; i++) L(on_stack(w[i]) ? 'STAK' : w[i]);
    const int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    LS(buf);
    return r;
}
static void __cdecl stub_LocaleConvertNumeric(char* s) { L('LCNV'); LS(s); }
static int __cdecl stub_tolower(int c) { L('TLOW'); L((uint32_t)c); return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int __cdecl stub_stricmp(const char* a, const char* b) {
    L('SICM'); LS(a); LS(b);
    if (!readable(a, 1) || !readable(b, 1)) { *(volatile int*)0 = 0; }
    const int r = _stricmp(a, b);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
// the fake file (in the arena): opened, sized, read in order, written (logged by hash)
static uint8_t* g_fb;                                        // the file's bytes (0x24000)
static int32_t __cdecl stub_FileOpen(const char* name) { L('FOPN'); LS(name); return HS->file_ok ? 0x1000 + HS->nopen++ : 0; }
static void __cdecl stub_FileClose(int32_t* fd) { L('FCLS'); L((uint32_t)*fd); *fd = 0; }
static int32_t __cdecl stub_FileCreate(const char* name) { L('FCRE'); LS(name); return HS->create_ok ? 0x2000 + HS->nopen++ : 0; }
static int32_t __cdecl stub_FileSize(int32_t fd) { L('FSIZ'); L((uint32_t)fd); return HS->file_size; }
static uint8_t __cdecl stub_FileReadExact(int32_t fd, void* buf, int32_t n) {
    L('FRDX'); L((uint32_t)fd); L(P(buf)); L((uint32_t)n);
    if (!HS->read_ok || n < 0 || HS->fpos + n > HS->flen || n > 0x24000) return 0;
    memcpy(buf, g_fb + HS->fpos, (size_t)n);
    HS->fpos += n;
    return 1;
}
static uint8_t __cdecl stub_FileWrite(int32_t fd, const void* p, int32_t n) {
    L('FWRT'); L((uint32_t)fd); L((uint32_t)n);
    if (n == 0x1cc && readable(p, 0x1cc)) {
        // a ghost file's header, as save() builds it on its stack: the bytes it never writes (after the track name's
        // terminator, +0x1e, +0x1f, and 0xec..0x1c0) are whatever the stack held -- left out, as the game never reads them
        uint8_t h[0x1cc];
        memcpy(h, p, 0x1cc);
        const size_t tl = strnlen((const char*)h + 0x10, 0xd);
        for (size_t i = 0x10 + tl + 1; i < 0x1d; i++) h[i] = 0;
        h[0x1e] = h[0x1f] = 0;
        memset(h + 0xec, 0, 0x1c0 - 0xec);
        L(hash_mem(h, 0x1cc));
    } else if (n >= 0 && n <= 0x40000 && readable(p, (uint32_t)(n ? n : 1))) L(hash_mem(p, (uint32_t)n));
    else L('BADW');
    return 1;
}
static void __cdecl stub_FileSetWritable(const char* s, uint32_t b) { L('FSWR'); LS(s); L(b & 0xff); }
static uint8_t __cdecl stub_FileRemove(const char* s) { L('FREM'); LS(s); return 1; }
static uint8_t __cdecl stub_FileCreateDirectory(const char* s) { L('FDIR'); LS(s); return 1; }
static void* __cdecl stub_FileFindFirst(const char* pat, char*, int n) { L('FFF '); LS(pat); L((uint32_t)n); return (void*)(intptr_t)-1; }
static uint8_t __cdecl stub_FileFindNext(void* h, char*, int) { L('FFN '); L((uint32_t)(uintptr_t)h); return 0; }
static void __cdecl stub_FileFindClose(void* h) { L('FFC '); L((uint32_t)(uintptr_t)h); }
static const char g_userdir[] = "C:\\Users\\player\\Viper\\";
static const char* __cdecl stub_Win32GetUserDirectory() { L('UDIR'); return g_userdir; }
// the file boxes
static uint8_t box(uint32_t tag, const char* title, const char* label, const char* pat, char* buf, int32_t n, const char* dir) {
    L(tag); LS(title); LS(label); LS(pat); L(P(buf)); L((uint32_t)n); LS(dir); LS(buf, 0x104);
    if (!HS->box_ok) return 0;
    const char* nm = (HS->box_ok & 2) ? "my replay.rpl" : "a.rpl";
    if (n > 0) { strncpy(buf, nm, (size_t)n - 1); buf[n - 1] = 0; }
    return 1;
}
static uint8_t __cdecl stub_UIDoOpenFileBox(const char* t, const char* l, const char* p, char* b, int32_t n, const char* d) { return box('OPNB', t, l, p, b, n, d); }
static uint8_t __cdecl stub_UIDoSaveFileBox(const char* t, const char* l, const char* p, char* b, int32_t n, const char* d) { return box('SAVB', t, l, p, b, n, d); }
static void __cdecl stub_UIDoOkBox(const char* t, const char* x) { L('OKBX'); LS(t); LS(x); }

// ---- stubs: this group's callees ----------------------------------------------------------------------------------------------------
static uint8_t* g_world;                                    // the game World the stubs answer for (arena)
#define LOG0(NAME, TAG) static void __cdecl NAME() { L(TAG); }
LOG0(stub_WorldSwitchToDrive, 'WSDR') LOG0(stub_WorldSwitchToUI, 'WSUI') LOG0(stub_WorldEndReplay, 'WERP')
LOG0(stub_WorldUpdate, 'WUPD') LOG0(stub_WorldFXEnable, 'FXEN') LOG0(stub_WorldFXDisable, 'FXDI')
LOG0(stub_TelemetryBegin, 'TELB') LOG0(stub_TelemetryEnd, 'TELE') LOG0(stub_SoundMuteCars, 'SMUT')
LOG0(stub_SoundUnMuteCars, 'SUNM') LOG0(stub_mrModelFlush, 'MRFL') LOG0(stub_SplashLoading, 'SPLL') LOG0(stub_move_blimp, 'BLMP')
LOG0(stub_ProfReset, 'PRST') LOG0(stub_ProfResetAndReport, 'PRRR') LOG0(stub_PhysReplayPlayBegin, 'RPPB')
LOG0(stub_PhysReplayPlayEnd, 'RPPE') LOG0(stub_PhysReplayRewind, 'RPRW') LOG0(stub_PhysReplayPause, 'RPPA')
LOG0(stub_PhysReplayResume, 'RPRE') LOG0(stub_PhysReplayFastForward, 'RPFF')
#undef LOG0
static void __cdecl stub_WorldBeginReplay(void* w) { L('WBRP'); L(P(w)); }
static void __cdecl stub_WorldDraw(uint32_t b) { L('WDRW'); L(b & 0xff); }
static void __cdecl stub_CameraSetType(int32_t t) { L('CAMT'); L((uint32_t)t); }
static void __cdecl stub_mrSetView(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) { L('MRSV'); L(x); L(y); L(w); L(h); L(c & 0xff); }
static void __cdecl stub_ResourceSetMustLoad(const char* s) { L('RSML'); LS(s); }
static void __cdecl stub_ResourceSetUnload(const char* s) { L('RSUL'); LS(s); }
static void __cdecl stub_LoadCars(void* w) { L('LCRS'); L(P(w)); }
static void __cdecl stub_UnloadCars(const void* w) { L('UCRS'); L(P(w)); }
static void __cdecl stub_LoadCar(const char* s) { L('LCAR'); LS(s); }
static void __cdecl stub_UnloadCar(const char* s) { L('UCAR'); LS(s); }
static void __cdecl stub_TaskDestroy(int32_t t) { L('TDES'); L((uint32_t)t); }
static void __cdecl stub_TaskResume(int32_t t) { L('TRES'); L((uint32_t)t); }
static void __cdecl stub_TaskSleep(int32_t t) { L('TSLP'); L((uint32_t)t); }
static uint8_t __cdecl stub_PhysReplayIsPaused() {
    L('RPIP');
    if (HS->frames > HS->frame_limit) return 1;
    return (uint8_t)((HS->paused_bits >> (HS->paused_n++ & 31)) & 1);
}
static uint8_t __cdecl stub_PhysReplayIsAtEnd() { L('RPAE'); return (uint8_t)((HS->atend_bits >> (HS->atend_n++ & 31)) & 1); }
static void __cdecl stub_PhysReplayShuttle(uint32_t b) { L('RPSH'); L(b); }
static uint8_t __cdecl stub_PhysReplayLoad(int32_t fd) { L('RPLD'); L((uint32_t)fd); return (uint8_t)HS->rload_ok; }
static void __cdecl stub_PhysReplaySave(int32_t fd) { L('RPSV'); L((uint32_t)fd); }
static uint8_t __cdecl stub_PhysReplayGetTelemetry(uint8_t* tel, uint32_t t) {
    L('RPTL'); L(t);
    const uint8_t ok = (uint8_t)((HS->tel_bits >> (HS->tel_n++ & 31)) & 1);
    if (ok) {
        memcpy(tel, "\x11\x22\x33\x44\x05\0\0\0", 8);
        memcpy(tel + 8, &HS->tel[(HS->tel_n) % 3], 12);
    }
    return ok;
}
static int32_t __cdecl stub_feed_ghost_car(int32_t car, int32_t start, int32_t dur, uint8_t* packets, int32_t* count) {
    L('FEED'); L((uint32_t)car); L((uint32_t)start); L((uint32_t)dur); L(P(packets)); L((uint32_t)*count);
    int32_t n = *count < HS->feed_n ? *count : HS->feed_n;
    if (n < 0) n = 0;
    for (int32_t i = 0; i < n * 60; i++) packets[i] = (uint8_t)(i * 7 + car);
    *count = n;
    return HS->feed_ret;
}
static void __cdecl stub_GhostCar_NewLap(const int32_t* gd, const void* packets, int32_t car) {
    L('GNLP'); L((uint32_t)gd[0]); L((uint32_t)gd[1]); L((uint32_t)gd[2]); L(P(packets)); L((uint32_t)car);
}
static int32_t __cdecl stub_AIGetMaxGhostSubmitTicks() { L('AIGT'); return HS->maxsubmit; }
static uint8_t __cdecl stub_MultiEnabled() { L('MEN '); return (uint8_t)HS->multi; }
static uint8_t __cdecl stub_MultiCarIsHuman(int32_t i) { L('MHUM'); L((uint32_t)i); return (uint8_t)((HS->human_mask >> (i & 31)) & 1); }
static int32_t __cdecl stub_WorldGetFocusCar() { L('WGFC'); return HS->focus; }
static int32_t __cdecl stub_WorldGetPlayerCar() { L('WGPC'); return HS->player; }
static void __cdecl stub_WorldSetFocusCar(int32_t c) { L('WSFC'); L((uint32_t)c); if (c >= 0 && c < 8) UI_G32(0x00521614) = c; }
static void __cdecl stub_WorldNextFocusCar() { L('WNFC'); UI_G32(0x00521614) = (HS->focus + 1) & 7; }
static void __cdecl stub_WorldPrevFocusCar() { L('WPFC'); UI_G32(0x00521614) = (HS->focus + 7) & 7; }
static const uint8_t* __cdecl stub_WorldGameOptions() { L('WGOP'); return g_world + W_REALISM; }
static const char* __cdecl stub_WorldGetTrackname() { L('WGTN'); return (const char*)(g_world + W_TRACK); }
static const uint8_t* __cdecl stub_WorldGetCarEntry(int32_t c) { L('WGCE'); L((uint32_t)c); return g_world + W_CARS + 0xc8u * (uint32_t)c; }
static const char* const k_cams[12] = {"In car", "Bumper", "X-ray", "Rear", "Near chase", "Chase", "Far chase", "Rear chase",
                                       "Overhead", "Aerial", "TV", "Blimp"};
static const char* __cdecl stub_GetCameraName(int32_t c) { L('CAMN'); L((uint32_t)c); return k_cams[(uint32_t)c % 12]; }
static const char* const k_tracks[] = {"bemidji", "heaven", "uptown", "hastings", "limbo", "dundas", "nfield", "kenyon", "hell"};
static int32_t __cdecl stub_GetTrackNumber(const char* name) {
    L('TNUM'); LS(name);
    if (!name || !readable(name, 1)) return -1;
    for (int i = 0; i < 8; i++)
        if (!_stricmp(k_tracks[i], name)) return i;
    return -1;
}
static char g_time[16][16];
static const char* __cdecl stub_PhysicsTimeString(uint32_t bits, uint32_t h) {
    L('PTST'); L(bits); L(h & 0xff);
    char* s = g_time[bits & 15];
    sprintf(s, "%u.%02u", (bits >> 12) & 0xfff, bits & 0x3f);
    return s;
}

// ---- the raw call ---------------------------------------------------------------------------------------------------------------------
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
    if (getenv("VP_DEBUG_WORLD"))
        printf("  fault at %08x: address %08x, eax %08x ecx %08x ebx %08x\n", (uint32_t)e->ContextRecord->Eip,
               (uint32_t)e->ExceptionRecord->ExceptionInformation[1], (uint32_t)e->ContextRecord->Eax, (uint32_t)e->ContextRecord->Ecx,
               (uint32_t)e->ContextRecord->Ebx);
    return EXCEPTION_EXECUTE_HANDLER;
}
// the stack the call will use, filled with one pattern (world_menu_options.cpp's: last before the call, top down)
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
    ReplayView* view;
    GraphControl* graph;
    JogControl* jog;                                          // a lone one (no widget)
    uint8_t* world;                                           // the game World (0xcd4)
    uint8_t* msg;                                             // CarMessages (4 x 0x80)
    LiveGhost* ghosts;                                        // [3]
    LiveGhost* best;
    LiveGhost* submit;
    uint8_t* ghostcar;                                        // a GhostCar's first 0xf00 bytes
    uint8_t* scratch;                                         // 0x1000
    uint8_t* bigscratch;                                      // a LiveGhost's size, for the constructors
    char* texts[8];
    int32_t* ints;
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
        const bool mine = !strcmp(c[3], "replay.obj") || !strcmp(c[3], "ghost.obj");
        if ((!strcmp(c[2], "ui") || mine) && c[5][0] == '$' && c[5][1] == 'E') g_setup_e.push_back((uint32_t)strtoul(c[0], 0, 16));
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
static const char* k_cars[] = {"viper", "vipergt", "cobra", "gts", "rt10", "stingray", "zr1"};
static void build_world() {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
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
    W.world = (uint8_t*)wv(W_SIZE);
    g_world = W.world;
    W.msg = (uint8_t*)wv(0x200);
    W.ghosts = (LiveGhost*)wv(3 * sizeof(LiveGhost));
    W.best = (LiveGhost*)wv(sizeof(LiveGhost));
    W.submit = (LiveGhost*)wv(sizeof(LiveGhost));
    W.ghostcar = (uint8_t*)wv(0xf00);
    W.scratch = (uint8_t*)wv(0x1000);
    W.bigscratch = (uint8_t*)wv(sizeof(LiveGhost));
    g_fb = (uint8_t*)wv(0x24000);
    for (int i = 0; i < 8; i++) W.texts[i] = (char*)wv(0x120);
    W.ints = (int32_t*)wv(64);
    for (int i = 0; i < 3; i++) call_orig(F_LiveGhost_ctor, 1, {U(&W.ghosts[i]), 0});
    call_orig(F_LiveGhost_ctor, 1, {U(W.best), 0});
    call_orig(F_LiveGhost_ctor, 1, {U(W.submit), 0});
    if (g_wv > A_HEAP) printf("the world overlaps the heap (%x)\n", g_wv);
    // the replay screen's control and a graph, in a full-screen window
    W.view = (ReplayView*)wv(sizeof(ReplayView));
    W.view->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    W.view->widget = 0;
    call_orig(F_JogControl_ctor, 1, {U(&W.view->jog), 0});
    W.view->vtbl = (const void*)(uintptr_t)VT_ReplayView;
    W.graph = (GraphControl*)wv(sizeof(GraphControl));
    W.graph->vtbl = (const void*)(uintptr_t)VT_GraphControl;
    W.jog = (JogControl*)wv(sizeof(JogControl));
    call_orig(F_JogControl_ctor, 1, {U(W.jog), 0});
    UIDialogItem* items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 4);
    UIDialogItem* it = items;
    it = it_(it, 0x17, 0, 0x7a, 0x88, 0x114, 0xcc, "", 0, W.view, 0);
    it = it_(it, 0x17, 0, 0x10, 0x300 - 0x2b0, 0x104, 0x52, "", 0, W.graph, 0);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    call_orig(F_WidgetCreateWindow, 0, {0, 0, 0, 640, 480, 2});
    W.win = (WidgetWindow*)(uintptr_t)g_setup_ret;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    call_orig(F_UIAddItems, 0, {U(W.win), U(items)});
    call_orig(0x00409640, 1, {U(W.view), 0});                  // ReplayView::Create
    call_orig(0x004093c0, 1, {U(&W.view->jog), 0});            // JogControl::Create (its notification)
    for (int i = 0; i < W.win->count; i++) W.widgets.push_back((Widget*)W.win->widgets[i]);
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
static const uint32_t k_xl[][4] = {
    {0x00505584, 0x01, 0x005053d0, 0x004e48e8}, {0x00505584, 0x02, 0x00505810, 0x004e48fc}, {0x00505584, 0x04, 0x00505680, 0x004e4910},
    {0x00505584, 0x08, 0x005053b0, 0x004e4928}, {0x00505584, 0x10, 0x00505440, 0x004e4940}, {0x0050539c, 0x01, 0x00505380, 0x004e4994},
    {0x00505438, 0x01, 0x00505460, 0x004e49a4}, {0x00505438, 0x02, 0x00505450, 0x004e49bc}, {0x00505824, 0x01, 0x00505588, 0x004e4a18},
    {0x00505824, 0x02, 0x00505428, 0x004e4a30}, {0x00505824, 0x04, 0x00505670, 0x004e4a4c}, {0x00505824, 0x08, 0x005053e0, 0x004e4a6c},
    {0x00505824, 0x10, 0x00505690, 0x004e4aa4}, {0x00505824, 0x20, 0x005055a8, 0x004e4adc}, {0x005053ac, 0x01, 0x00505658, 0x004e4b50},
    {0x005053ac, 0x02, 0x005053c0, 0x004e4b68}, {0x005053ac, 0x04, 0x00504698, 0x004e4b84}, {0x005053ac, 0x08, 0x00504660, 0x004e4ba4},
    {0x00505608, 0x01, 0x00505648, 0x004e4be0}, {0x00505608, 0x02, 0x005057f0, 0x004e4bf4}, {0x00505608, 0x04, 0x00504680, 0x004e4c08},
    {0x00505608, 0x08, 0x00505410, 0x004e4c20}, {0x00505608, 0x10, 0x00504670, 0x004e4c34}, {0x00505608, 0x20, 0x005057d8, 0x004e4c48},
    {0x00505608, 0x40, 0x00505578, 0x004e4c5c}, {0x00505608, 0x80, 0x00505628, 0x004e4c74}, {0x0050541c, 0x01, 0x00505598, 0x004e4cb8},
    {0x0050541c, 0x02, 0x00505618, 0x004e4cd8}, {0x0050541c, 0x04, 0x00505638, 0x004e4d10}, {0x0050541c, 0x08, 0x005057c8, 0x004e4d48},
};
static const char k_chars[] = "abcdefghij klmnopqrstuvwxyz ABCDEFGH 0123456789 .,!-";
static void random_text(char* s, int maxlen) {
    const int n = irange(0, maxlen);
    for (int i = 0; i < n; i++) s[i] = k_chars[rnd() % (sizeof k_chars - 1)];
    s[n] = 0;
}
static void random_track(char* s) {
    if (chance(85)) strcpy(s, k_tracks[rnd() % 9]);
    else random_text(s, 12);
}
static void random_packets(uint8_t* p, int32_t n) {
    float x = range(-500, 500), y = range(-10, 10), z = range(-500, 500);
    for (int32_t i = 0; i < n; i++) {
        uint8_t* q = p + 60 * i;
        for (int k = 0; k < 60; k++) q[k] = (uint8_t)rnd();
        if (chance(3)) { x += range(-20000, 20000); }
        else { x += range(-50, 50); y += range(-1, 1); z += range(-50, 50); }
        float v[3] = {x, y, z};
        if (chance(2)) v[rnd() % 3] = wildf();
        memcpy(q + 6, v, 12);
    }
}
static void random_header(uint8_t* h, bool full) {
    const char* trk = (const char*)(g_world + W_TRACK);
    uint32_t ver = 3;
    for (int i = 0; i < 8; i++) if (!_stricmp(trk, k_tracks[i])) ver = UI_GU32(S_GF_VERSIONS + 4u * (uint32_t)i);
    *(uint32_t*)(h + 0) = chance(85) ? ver : (uint32_t)irange(0, 6);
    *(int32_t*)(h + 4) = chance(90) ? 0x960 : irange(0, 0x1000);
    *(int32_t*)(h + 8) = chance(85) ? (int32_t)(0x960 * 60 + 0x1cc) : chance(50) ? (int32_t)(0x960 * 60 + 0x1cc - 4) : (int32_t)rnd();
    *(int32_t*)(h + 0xc) = chance(85) ? *(int32_t*)(g_world + W_REALISM) : irange(0, 3);
    memset(h + 0x10, 0, 0xd);
    if (chance(85)) strcpy((char*)h + 0x10, trk);
    else random_text((char*)h + 0x10, 12);
    h[0x1d] = (uint8_t)(rnd() & 1);
    uint8_t* e = h + 0x20;
    *(int32_t*)e = irange(0, 3);
    for (int k = 4; k < 0x11; k++) e[k] = (uint8_t)rnd();
    memset(e + 0x11, 0, 0x23);
    strcpy((char*)e + 0x11, k_cars[rnd() % 7]);
    *(float*)(h + 0xe8) = fval(0, 200);
    *(int32_t*)(h + 0x1c0) = chance(85) ? irange(100, 20000) : (int32_t)rnd();
    *(int32_t*)(h + 0x1c4) = irange(-100, 5000);
    *(int32_t*)(h + 0x1c8) = full ? irange(1, 48) : irange(0, 48);
}
static void randomize_world() {
    // the game World: the car list and the options
    uint8_t* w = W.world;
    *(int32_t*)w = chance(90) ? 3 : irange(0, 5);
    *(int32_t*)(w + 4) = chance(90) ? (int32_t)W_SIZE : irange(0, 0x2000);
    memset(w + W_TRACK, 0, 0x20);
    random_track((char*)w + W_TRACK);
    const int ncars = chance(90) ? irange(1, 6) : irange(0, 8);
    *(int32_t*)(w + W_NCARS) = ncars;
    for (int i = 0; i < 16; i++) {
        uint8_t* e = w + W_CARS + 0xc8 * i;
        *(int32_t*)e = i == 0 && chance(85) ? 0 : chance(70) ? 1 : irange(0, 3);
        for (int k = 4; k < 0x11; k++) e[k] = (uint8_t)rnd();
        memset(e + 0x11, 0, 0x23);
        strcpy((char*)e + 0x11, k_cars[chance(50) ? 0 : rnd() % 7]);
    }
    *(int32_t*)(w + W_REALISM) = chance(90) ? irange(0, 2) : irange(0, 3);
    *(int32_t*)(w + W_FIELD) = irange(0, 2);
    *(int32_t*)(w + W_RACE_TYPE) = irange(0, 6);
    *(int32_t*)(w + W_EVENT) = chance(70) ? 0 : irange(0, 3);
    *(int32_t*)(w + W_GHOST) = chance(90) ? irange(0, 3) : irange(-1, 5);
    w[W_MIRROR] = (uint8_t)(chance(80) ? rnd() & 1 : rnd());
    // the car managers
    for (int i = 0; i < 16; i++) {
        uint8_t* info = (uint8_t*)(uintptr_t)(0x00554090 + 0x170 * i);
        *(int32_t*)info = chance(70) ? 0 : irange(1, 2);
        info[4] = (uint8_t)(chance(50) ? 1 : 0);
        memset(info + 5, 0, 0xd);
        random_text((char*)info + 5, chance(90) ? 10 : 12);
        *(uint8_t**)(info + 0x134) = W.msg + 0x80 * (i & 3);
    }
    for (int i = 0; i < 4; i++) {
        float* m = (float*)(W.msg + 0x80 * i);
        for (int k = 0; k < 32; k++) m[k] = fval(-80, 80);
        ((int32_t*)m)[0x34 / 4] = irange(-1, 6);
    }
    // the ghosts
    for (int g = 0; g < 5; g++) {
        LiveGhost* lg = g < 3 ? &W.ghosts[g] : g == 3 ? W.best : W.submit;
        lg->tried = (uint8_t)chance(40);
        lg->valid = (uint8_t)chance(50);
        lg->dirty = (uint8_t)chance(50);
        lg->car = chance(90) ? irange(0, 7) : irange(0, 15);
        lg->best = chance(20) ? 0x7fffffff : irange(100, 20000);
        random_header((uint8_t*)&lg->h, true);
        if (chance(30)) lg->h.ticks = lg->best;
    }
    // the fake file: a replay World, or a ghost file
    const bool ghostfile = chance(50);
    memset(g_fb, 0, 0x2000);
    int32_t len;
    if (ghostfile) {
        random_header(g_fb, true);
        const int32_t n = *(int32_t*)(g_fb + 0x1c8);
        random_packets(g_fb + 0x1cc, n > 48 || n < 0 ? 48 : n);
        len = 0x1cc + 60 * (n > 48 || n < 0 ? 48 : n);
    } else {
        memcpy(g_fb, w, W_SIZE);
        if (chance(15)) *(int32_t*)g_fb = irange(0, 5);
        len = W_SIZE + 0x40;
    }
    HS->flen = chance(90) ? len : irange(0, len);
    HS->file_size = chance(80) ? HS->flen : chance(50) ? 0 : chance(50) ? (int32_t)(0x960 * 60 + 0x1cc) : irange(-5, 0x30000);
    HS->fpos = 0;
    HS->file_ok = chance(85);
    HS->read_ok = chance(90);
    HS->create_ok = chance(85);
    HS->box_ok = chance(75) ? irange(1, 3) : 0;
    HS->rload_ok = chance(70);
    HS->nopen = 0;
    // the stubs' answers
    HS->time = (int32_t)rnd();
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->grab_fail_at = -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->script_n = HS->script_pos = 0;
    HS->paused_bits = rnd(); HS->paused_n = 0;
    HS->atend_bits = chance(70) ? 0 : rnd(); HS->atend_n = 0;
    HS->scan_bits = chance(50) ? 0 : rnd(); HS->scan_n = 0;
    HS->tel_bits = rnd(); HS->tel_n = 0;
    for (int i = 0; i < 5; i++) HS->tel[i] = fval(-6, 6);
    HS->focus = chance(90) ? irange(0, 7) : irange(0, 15);
    HS->player = irange(-1, 8);
    HS->multi = chance(25);
    HS->human_mask = (int32_t)(rnd() & 0x3f);
    HS->maxsubmit = irange(0, 30000);
    HS->feed_n = irange(0, 48);
    HS->feed_ret = irange(-10, 100);
    g_xl_seed = chance(80) ? 0 : rnd();
    for (Widget* g : W.widgets) { g->visible = chance(85); g->enabled = chance(85); g->dirty = chance(50); }
    for (int i = 0; i < 8; i++) random_text(W.texts[i], i < 4 ? 20 : 60);
    for (int i = 0; i < 16; i++) W.ints[i] = irange(-2, 12);
    // the controls
    for (RvCtl* c : {(RvCtl*)W.view, (RvCtl*)&W.view->jog, (RvCtl*)W.graph, (RvCtl*)W.jog}) {
        if (chance(30)) { c->x = irange(-20, 600); c->y = irange(-20, 440); c->w = chance(95) ? irange(4, 400) : irange(-8, 3); c->h = irange(-8, 300); }
    }
    for (JogControl* j : {&W.view->jog, W.jog}) j->dragging = (uint8_t)chance(60);
}
// .data after poisoning, and each round: the statics the code follows
static void randomize_statics() {
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_GU32(S_DIALOG_IDLE) = 0;
    UI_G8(S_EXIT) = 0;
    UI_GF(S_UI_DT) = chance(10) ? wildf() : range(0.0f, 0.25f);
    UI_GP(void, S_CURSOR) = 0;
    UI_G32(S_MOUSE_X) = irange(0, 639); UI_G32(S_MOUSE_Y) = irange(0, 479);
    UI_G32(S_SCREEN_W) = chance(90) ? 640 : irange(400, 1600);
    UI_G32(S_SCREEN_H) = 480;
    for (auto& x : k_xl) {
        if (chance(75)) {
            UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) | x[1]);
            UI_GU32(x[2]) = x[3];
            UI_GU32(x[2] + 4) = U(xl_text(x[3]));
            UI_GU32(x[2] + 8) = chance(70) ? UI_GU32(S_XLATOR_COOKIE) : ~UI_GU32(S_XLATOR_COOKIE);
        } else {
            UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) & ~x[1]);
        }
    }
    // replay.obj
    UI_G8(S_HAVE_REPLAY) = (uint8_t)chance(70);
    UI_G8(S_FROM_RACE) = (uint8_t)chance(30);
    UI_G8(S_AUTOLOAD) = (uint8_t)chance(15);
    UI_G32(S_SHUTTLE) = chance(50) ? 0 : chance(50) ? -1 : 1;
    UI_G32(S_SHUTTLE_LAST) = chance(50) ? UI_G32(S_SHUTTLE) : irange(-1, 1);
    UI_GF(S_SHUTTLE_SPEED) = fval(1, 10);
    UI_G32(S_CAMERA) = chance(90) ? irange(0, 11) : (int32_t)rnd();
    UI_G32(S_GRAPH_TYPE) = chance(90) ? irange(0, 5) : irange(-1, 6);
    UI_GF(S_GRAPH_ZOOM) = fval(0.2f, 10);
    UI_GF(S_GRAPH_SCALE) = fval(0, 3);
    UI_G32(S_FRAME) = (int32_t)rnd();
    UI_G32(S_FRAME8) = irange(-2, 9);
    UI_G32(S_BENCH_FRAMES) = (int32_t)rnd();
    memset((void*)(uintptr_t)S_REPLAY_FILE, 0, 0x104);
    if (chance(40)) random_text((char*)(uintptr_t)S_REPLAY_FILE, 30);
    memset((void*)(uintptr_t)S_REPLAY_DIR, 0, 0x108);
    strcpy((char*)(uintptr_t)S_REPLAY_DIR, "C:\\Users\\player\\Viper\\replay\\");
    memset((void*)(uintptr_t)(S_REPLAY_WORLD + W_TRACK), 0, 0x20);
    random_track((char*)(uintptr_t)(S_REPLAY_WORLD + W_TRACK));
    UI_G32(S_REPLAY_WORLD) = chance(90) ? 3 : 2;
    UI_G32(S_REPLAY_WORLD + 4) = (int32_t)W_SIZE;
    UI_GP(void, S_VIEW) = W.view;
    // the texts the replay screen's static texts show (replay_idle keeps them terminated; .bss starts them empty)
    memset((void*)(uintptr_t)S_CAR_NAME, 0, 0x20);
    random_text((char*)(uintptr_t)S_CAR_NAME, 12);
    memset((void*)(uintptr_t)S_CAMERA_NAME, 0, 0x20);
    random_text((char*)(uintptr_t)S_CAMERA_NAME, 12);
    memset((void*)(uintptr_t)S_TIME_TEXT, 0, 0x50);
    random_text((char*)(uintptr_t)S_TIME_TEXT, 12);
    // ghost.obj
    UI_G32(S_TARGET) = chance(80) ? irange(0, 2) : -1;
    UI_G8(S_INCOGNITO) = (uint8_t)(rnd() & 1);
    UI_G32(S_NGHOSTS) = irange(1, 3);
    UI_GP(void, S_GHOSTS) = chance(95) ? (void*)W.ghosts : 0;
    UI_GP(void, S_BEST) = chance(90) ? (void*)W.best : 0;
    UI_GP(void, S_SUBMIT) = chance(10) ? (void*)W.submit : 0;
    UI_GP(void, S_CURRENT) = chance(20) ? 0 : chance(50) ? (void*)&W.ghosts[rnd() % 3] : (void*)W.best;
    UI_G32(S_TASK) = chance(80) ? 0 : 0x77;
    for (int i = 0; i < 16; i++) UI_G32(S_SLOTS + 4 * i) = chance(85) ? irange(0, UI_G32(S_NGHOSTS) - 1) : 0x7fffffff;
    UI_G32(S_MAX_PACKETS) = 0x960;
    UI_G32(S_FILE_SIZE) = 0x960 * 60 + 0x1cc;
    UI_G32(0x0052161c) = irange(0, 100000);                  // physics_tick
    UI_GP(void, 0x0052197c) = chance(70) ? (void*)W.ghostcar : 0;
    UI_G32(0x00521614) = irange(0, 7);
}
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 0x70, 0x121, 0x122, 0x124, 0x128, 0x170, 0x175, 0x17b, 0x325, 0x327, '1', '3', 'q'};
    const int n = irange(0, maxops);
    for (int i = 0; i < n && i < 62; i++) {
        int32_t* op = HS->script[i];
        const uint32_t r = rnd() % 10;
        op[1] = op[2] = 0;
        if (r < 4) { op[0] = OP_MOVE_W; op[1] = irange(0, 60); op[2] = irange(0, 479); }
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
        {0x004e4db8, 0x004e4e40},             // replay.obj's strings (and "")
        {0x004e48a8, 0x004e4db8},
        {0x004e5400, 0x004e5740},             // ghost.obj's version table, names, strings
        {0x004eb108, 0x004eb10c},             // Xlator::g_cookie
        {0x004f665c, 0x004f6698},             // the ui library's Xlators' keys
        {0x004f6b78, 0x004f7228}, {0x004f7230, 0x004f7300},   // ui / _widget / widget / uistyle strings
        {0x004efbf8, 0x004efbfc},             // the current canvas
        {0x004f48d0, 0x004f48d4},             // CarMgrCount
        {0x005024b8, 0x005024bc},             // __adjust_fdiv
        {0x00502630, 0x00502680},             // _CIfmod's descriptor
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
    // the CRT's own data the math routines follow (_CIfmod's dispatch, the FP state): everything the image had there
    memcpy(DATA + (0x00502000 - 0x004e1000), keepfrom.data + (0x00502000 - 0x004e1000), 0x1000);
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static uint32_t b8(uint32_t v) { return chance(50) ? (rnd() & ~0xffu) | (v & 0xff) : v; }
static uint32_t canvas_arg() { return chance(70) ? U(W.screen) : U(W.canvas2); }
static uint32_t name_arg() { return chance(70) ? U(k_cars[rnd() % 7]) : U(W.texts[rnd() % 4]); }
static uint32_t track_arg() { return chance(80) ? U(W.world + W_TRACK) : chance(50) ? U(k_tracks[rnd() % 9]) : U(W.texts[rnd() % 4]); }
static uint32_t ghost_arg() {
    switch (rnd() % 5) {
    case 0: case 1: case 2: return U(&W.ghosts[rnd() % 3]);
    case 3: return U(W.best);
    default: return U(W.submit);
    }
}
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const char* nm = f.name;
    uint32_t* a = f.fast ? w + 2 : w;
    memset(W.scratch, 0xa5, 0x1000);
#define IS(s) (!strcmp(nm, s))
#define HAS(s) (strstr(nm, s) != 0)
    if (HAS("deleting destructor")) { a[0] = rnd() & 3; }
    if (HAS("UICustomControl::")) {
        w[0] = chance(50) ? U(W.scratch) : U(W.view);
        if (HAS("deleting")) { w[0] = U(W.scratch); *(uint32_t*)W.scratch = VT_GraphControl; }
    } else if (HAS("JogControl::")) {
        w[0] = chance(60) ? U(&W.view->jog) : U(W.jog);
        if (IS("JogControl::JogControl")) w[0] = U(W.scratch);
        else if (HAS("Draw")) a[0] = canvas_arg();
        else if (HAS("Mouse")) { a[0] = (uint32_t)irange(-100, 700); a[1] = (uint32_t)irange(-100, 500); }
        else if (HAS("Callback")) { a[0] = rnd() & 3; a[1] = U(W.scratch); }
        else if (HAS("deleting")) { memcpy(W.scratch, W.jog, sizeof(JogControl)); w[0] = U(W.scratch); }
    } else if (HAS("ReplayView::")) {
        w[0] = U(W.view);
        if (HAS("Draw") && !HAS("Draw3D")) a[0] = canvas_arg();
        else if (HAS("deleting")) { memcpy(W.scratch, W.view, sizeof(ReplayView)); w[0] = U(W.scratch); }
    } else if (HAS("GraphControl::")) {
        w[0] = U(W.graph);
        if (HAS("Draw")) a[0] = canvas_arg();
        else if (HAS("CharHit")) { static const char cs[] = " 12345 60a!\x7f\x80"; a[0] = chance(80) ? (rnd() & ~0xffu) | (uint8_t)cs[rnd() % 14] : rnd(); }
        else if (HAS("deleting")) { memcpy(W.scratch, W.graph, sizeof(GraphControl)); w[0] = U(W.scratch); }
    } else if (IS("UIDialogItem::UIDialogItem")) {
        w[0] = U(W.scratch);
    } else if (IS("UI_BBUTTON") || IS("UI_BREPEATBUTTON") || IS("UI_CUSTOM")) {
        a[0] = U(W.scratch);
    } else if (IS("ReplayDo")) {
        a[0] = chance(50) ? 0 : U(W.world);
    } else if (IS("replay_idle")) {
        a[0] = U(&W.ints[0]);
    } else if (IS("auto_load_replay") || IS("ReplayBenchmark")) {
        a[0] = chance(80) ? U("replays\\demo.rpl") : U(W.texts[4]);
    } else if (HAS("_cb")) {
        a[0] = rnd();
    // ---- ghost.obj
    } else if (IS("munge_carname_to_4char")) { a[0] = U(W.scratch); a[1] = name_arg(); }
    else if (IS("GhostBegin") || IS("add_ghost_car") || IS("find_target") || IS("load_old_stats") || IS("GhostEnd")) a[0] = U(W.world);
    else if (IS("set_ghostcar_entry")) { a[0] = U(W.world + W_CARS + 0xc8 * irange(3, 7)); a[1] = U(W.world + W_CARS + 0xc8 * irange(0, 2)); }
    else if (IS("initialize")) {
        a[0] = ghost_arg(); a[1] = (uint32_t)irange(0, 3); a[2] = (uint32_t)irange(0, 2); a[3] = b8(rnd() & 1); a[4] = track_arg(); a[5] = b8(rnd() & 1);
        a[6] = chance(80) ? name_arg() : 0;
    } else if (IS("load")) {
        a[0] = name_arg(); a[1] = (uint32_t)irange(0, 3); a[2] = (uint32_t)irange(0, 2); a[3] = b8(rnd() & 1); a[4] = track_arg(); a[5] = b8(rnd() & 1);
    } else if (IS("get_full_pathname") || IS("get_filename")) {
        a[0] = U(W.scratch); a[1] = name_arg(); a[2] = (uint32_t)irange(0, 3); a[3] = (uint32_t)(chance(90) ? irange(0, 2) : irange(0, 5));
        a[4] = b8(rnd() & 1); a[5] = track_arg(); a[6] = b8(rnd() & 1);
    } else if (IS("pre_worldbegin_is_ghostable_car")) { a[0] = U(W.world + W_CARS + 0xc8 * irange(0, 7)); a[1] = (uint32_t)irange(0, 7); }
    else if (IS("try_saving") || IS("save(LiveGhost*,GhostCarType)")) { a[0] = ghost_arg(); a[1] = (uint32_t)irange(2, 3); }
    else if (IS("save(LiveGhost*,char const*)")) { a[0] = ghost_arg(); a[1] = U("C:\\ghostcar\\bemiviper.gcf"); }
    else if (IS("gf_version")) a[0] = track_arg();
    else if (IS("convert_ticks_to_time")) a[0] = chance(80) ? (uint32_t)irange(0, 100000) : rnd();
    else if (IS("is_savable_ghost") || IS("is_ghostable_car")) a[0] = (uint32_t)irange(0, 15);
    else if (IS("remove_ghostcar")) a[0] = U(W.world + W_CARS);
    else if (IS("GetGhostInfo")) a[0] = U(W.scratch);
    else if (IS("GhostNewBestLap")) { a[0] = (uint32_t)irange(0, 7); a[1] = (uint32_t)(chance(90) ? irange(100, 20000) : irange(-100, 40000)); }
    else if (IS("GhostGetData")) { a[0] = U(W.scratch); a[1] = U(W.scratch + 0x10); a[2] = U(W.scratch + 0x20); }
    else if (IS("GhostLoad")) { a[0] = U("C:\\ghostcar\\bemaviper.gcf"); a[1] = b8(chance(30) ? 1 : 0); }
    else if (IS("ghost_appears_valid")) {
        uint8_t* gf = W.bigscratch;
        random_header(gf, true);
        random_packets(gf + 0x1cc, 48);
        *(int32_t*)(gf + 0x1c8) = irange(0, 48);
        a[0] = U(gf);
    } else if (IS("point_is_normal")) {
        float* p = (float*)W.scratch;
        for (int i = 0; i < 3; i++) p[i] = fval(-1000, 1000);
        a[0] = U(p);
    } else if (IS("LiveGhost::LiveGhost")) w[0] = U(W.bigscratch);
    else if (IS("Car::ReplayPacket::ReplayPacket")) w[0] = U(W.scratch);
    else if (HAS("ASSERT_MSG") || HAS("VERBOSE")) { a[0] = rnd(); a[1] = U("fmt %d"); }
#undef IS
#undef HAS
    return true;
}
static bool is_modal(const char* nm) {
    static const char* const m[] = {"ReplayDo", "fullscreen_cb", "do_analysis", "ReplayLoadDialog", "load_cb", "save_cb", "benchmark_loop",
                                    "ReplayBenchmark", "auto_load_replay", "replay_idle"};
    for (const char* s : m) if (!strcmp(nm, s)) return true;
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
    const char* only = getenv("VP_ONLY");
    char exe[MAX_PATH], inv[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_root_replay.cpp");
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
        {F_MemAlloc, (void*)&stub_MemAlloc}, {F_Delete, (void*)&stub_delete}, {F_MemFree, (void*)&stub_MemFree},
        {F_LogReport, (void*)&stub_LogReport}, {F_LogPanic, (void*)&stub_LogPanic}, {F_SingleBegin, (void*)&stub_SingleBegin},
        {F_SingleEnd, (void*)&stub_SingleEnd}, {F_atexit, (void*)&stub_atexit}, {F_PTimeNow, (void*)&stub_PTimeNow},
        {F_MouseGetEvent, (void*)&stub_MouseGetEvent}, {F_MousePeekEvent, (void*)&stub_MousePeekEvent},
        {F_KeyHit, (void*)&stub_KeyHit}, {F_KeyGet, (void*)&stub_KeyGet}, {F_MousePeek, (void*)&stub_MousePeek},
        {F_MouseClear, (void*)&stub_MouseClear}, {F_KeyClear, (void*)&stub_KeyClear}, {F_ScanDown, (void*)&stub_ScanDown},
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
        {F_LocaleConvertNumeric, (void*)&stub_LocaleConvertNumeric}, {F_tolower, (void*)&stub_tolower}, {F_stricmp, (void*)&stub_stricmp},
        {F_FileFindFirst, (void*)&stub_FileFindFirst}, {F_FileFindNext, (void*)&stub_FileFindNext},
        {F_FileFindClose, (void*)&stub_FileFindClose}, {F_FileOpen, (void*)&stub_FileOpen}, {F_FileClose, (void*)&stub_FileClose},
        {F_FileCreate, (void*)&stub_FileCreate}, {F_FileSize, (void*)&stub_FileSize}, {F_FileReadExact, (void*)&stub_FileReadExact},
        {F_FileWrite, (void*)&stub_FileWrite}, {F_FileSetWritable, (void*)&stub_FileSetWritable},
        {F_FileRemove, (void*)&stub_FileRemove}, {F_FileCreateDirectory, (void*)&stub_FileCreateDirectory},
        {F_Win32GetUserDirectory, (void*)&stub_Win32GetUserDirectory},
        {F_UIDoOpenFileBox, (void*)&stub_UIDoOpenFileBox}, {F_UIDoSaveFileBox, (void*)&stub_UIDoSaveFileBox},
        {F_UIDoOkBox, (void*)&stub_UIDoOkBox},
        // this group's callees
        {F_WorldSwitchToDrive, (void*)&stub_WorldSwitchToDrive}, {F_WorldSwitchToUI, (void*)&stub_WorldSwitchToUI},
        {F_WorldBeginReplay, (void*)&stub_WorldBeginReplay}, {F_WorldEndReplay, (void*)&stub_WorldEndReplay},
        {F_WorldUpdate, (void*)&stub_WorldUpdate}, {F_WorldDraw, (void*)&stub_WorldDraw},
        {F_WorldFXEnable, (void*)&stub_WorldFXEnable}, {F_WorldFXDisable, (void*)&stub_WorldFXDisable},
        {F_WorldGetFocusCar, (void*)&stub_WorldGetFocusCar}, {F_WorldGetPlayerCar, (void*)&stub_WorldGetPlayerCar},
        {F_WorldSetFocusCar, (void*)&stub_WorldSetFocusCar}, {F_WorldNextFocusCar, (void*)&stub_WorldNextFocusCar},
        {F_WorldPrevFocusCar, (void*)&stub_WorldPrevFocusCar}, {F_WorldGameOptions, (void*)&stub_WorldGameOptions},
        {F_WorldGetTrackname, (void*)&stub_WorldGetTrackname}, {F_WorldGetCarEntry, (void*)&stub_WorldGetCarEntry},
        {F_TelemetryBegin, (void*)&stub_TelemetryBegin}, {F_TelemetryEnd, (void*)&stub_TelemetryEnd},
        {F_SoundMuteCars, (void*)&stub_SoundMuteCars}, {F_SoundUnMuteCars, (void*)&stub_SoundUnMuteCars},
        {F_mrSetView, (void*)&stub_mrSetView}, {F_mrModelFlush, (void*)&stub_mrModelFlush},
        {F_CameraSetType, (void*)&stub_CameraSetType}, {F_PhysicsTimeString, (void*)&stub_PhysicsTimeString},
        {F_ResourceSetMustLoad, (void*)&stub_ResourceSetMustLoad}, {F_ResourceSetUnload, (void*)&stub_ResourceSetUnload},
        {F_LoadCars, (void*)&stub_LoadCars}, {F_UnloadCars, (void*)&stub_UnloadCars}, {F_LoadCar, (void*)&stub_LoadCar},
        {F_UnloadCar, (void*)&stub_UnloadCar}, {F_SplashLoading, (void*)&stub_SplashLoading}, {F_move_blimp, (void*)&stub_move_blimp},
        {F_GetCameraName, (void*)&stub_GetCameraName}, {F_GetTrackNumber, (void*)&stub_GetTrackNumber},
        {F_TaskDestroy, (void*)&stub_TaskDestroy}, {F_TaskResume, (void*)&stub_TaskResume}, {F_TaskSleep, (void*)&stub_TaskSleep},
        {F_ProfReset, (void*)&stub_ProfReset}, {F_ProfResetAndReport, (void*)&stub_ProfResetAndReport},
        {F_PhysReplayPlayBegin, (void*)&stub_PhysReplayPlayBegin}, {F_PhysReplayPlayEnd, (void*)&stub_PhysReplayPlayEnd},
        {F_PhysReplayRewind, (void*)&stub_PhysReplayRewind}, {F_PhysReplayPause, (void*)&stub_PhysReplayPause},
        {F_PhysReplayResume, (void*)&stub_PhysReplayResume}, {F_PhysReplayIsPaused, (void*)&stub_PhysReplayIsPaused},
        {F_PhysReplayIsAtEnd, (void*)&stub_PhysReplayIsAtEnd}, {F_PhysReplayFastForward, (void*)&stub_PhysReplayFastForward},
        {F_PhysReplayShuttle, (void*)&stub_PhysReplayShuttle}, {F_PhysReplayLoad, (void*)&stub_PhysReplayLoad},
        {F_PhysReplaySave, (void*)&stub_PhysReplaySave}, {F_PhysReplayGetTelemetry, (void*)&stub_PhysReplayGetTelemetry},
        {F_feed_ghost_car, (void*)&stub_feed_ghost_car}, {F_GhostCar_NewLap, (void*)&stub_GhostCar_NewLap},
        {F_AIGetMaxGhostSubmitTicks, (void*)&stub_AIGetMaxGhostSubmitTicks},
        {F_MultiEnabled, (void*)&stub_MultiEnabled}, {F_MultiCarIsHuman, (void*)&stub_MultiCarIsHuman},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
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
    int differ = 0, fp_bad = 0, faults = 0, pure_n = 0, replay_n = 0, changed_fns = 0, still_fns = 0, bad_fns = 0, fpck_fns = 0;
    long long both_fault = 0;
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0, fn_fpck = 0;
        const int nr = is_modal(f.name) ? rounds : rounds * 4;
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            if (poisoned) poison(g_pristine);
            randomize_world();
            randomize_statics();
            random_script(is_modal(f.name) ? 14 : 4);
            uint32_t words[72];
            make_args(f, words);
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            mem_save(g_snap);
            const Result ro = run(f, false, words);
            const bool changed = mem_diff(g_snap) != 0;
            fn_changed |= changed;
            if (!fp.replay_only && !ro.fault) {
                fn_fpck++;
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
        fpck_fns += fn_fpck > 0;
        (fn_changed ? changed_fns : still_fns)++;
        if (trace)
            printf("%08x %-48s %s%s (%d checks, %d with the footprint checked, %d faulted)\n", f.v10, f.name, fn_bad ? "BAD" : "ok",
                   fn_changed ? "" : " (never changed memory)", fn_checks, fn_fpck, fn_faults);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    printf("%d functions (%d pure, %d replay_only in round 0, %d others; %d with a footprint checked), %d listed twice; %lld checks "
           "(%lld on poisoned .data), %lld logged words compared: %d differ, %d footprint violations, %d checks faulted (%lld in both, "
           "the same way); %d functions bad\n", g_nfns, pure_n, replay_n, g_nfns - pure_n - replay_n, fpck_fns, dup, checks, poisoned_checks,
           log_words, differ, fp_bad, faults, both_fault, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    return differ || fp_bad || dup ? 1 : 0;
}
