// world_menu_multi.cpp -- multiplayer stage N3 (group A): every rewrite of hook/menu_multi.cpp (mmulti.obj, the
// Multiplayer screen) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_menu_multi.cpp
//        /Fo<dir>\ /Fe<dir>\world_menu_multi.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//     (and with /DVP_MENU_FIXES: the fix build, below)
//   run:   world_menu_multi.exe [rounds] [seed]      (VP_TRACE=1: one line per function; VP_ONLY=name: just those;
//                                                     VP_TAGS=1: which stubs the originals reached, how they returned)
//
// Built as test/world_menu_options.cpp is (its loader, raw call, memory comparison, the toolkit's stubs and logs are copied
// here): out\race_v10.exe loaded at 0x400000, hook/menu_multi.cpp included with PORT_FN redefined to list each function
// (its v1.0 address, the rewrite, its calling convention, stack arguments, return, footprint). VP_FAITHFUL: every rewrite
// must match its original bit for bit.
//
// The world. Once, at start: every $E static initialiser of the libraries `ui` and `menu` (the Xlators, the colours; read
// from out\inventory.csv), the game's own UIBegin, a full-screen window made by WidgetCreateWindow and made the running
// one, and in it -- by the game's own _UIAddItems -- the three tabs' controls, each built by its ORIGINAL constructor: a
// NetBrowser (its two SessionMgrs the stubs' fakes), a ModemLineControl and a DirectLineControl (their devices the stubs'
// LineEnumerateDevices'). Their Addeds (the originals) fill the window with their widgets. This is the pristine state.
//
// Then for EVERY listed function, `rounds` times (default 24; ten times that for one that runs no dialog): pristine
// restored; half the rounds POISONED (every byte of .data/.bss random, except what's read as a string or followed as a
// pointer: every dword the pristine state holds that points into the image, the arena or this program, the ui / menu
// strings, mmulti's strings and its CreateSocket table, the screen's size, __adjust_fdiv); the controls' state random
// (the protocol, the managers present or not, the games listed with versions older / the same / newer, passwords, the
// selection, the timers against the clock, the line devices -- in use or not, made or not --, the device's statuses, the
// line checker's counts and verdicts, the tab and the tabs' groups, the cancel box's idle function), the widgets' flags
// random, the stubs' world random (the options present or missing, sockets and managers failing, the host lookup's end
// state, the clock's step); arguments made for the function (the right object for a method, a canvas, a callback's id,
// a deleting destructor's flags, fresh random blocks for the constructors, a host and port, input boxes' labels / buffers /
// sizes, a cancel box's idle function); for a function that runs a dialog a random input script -- mouse moves onto the
// running window's widgets, presses and releases, keys (Enter, Escape, the arrows, text, ':'), idle frames -- ended by a
// watchdog after 40 frames if the dialog doesn't end itself. Snapshot; the stack below is filled with one pattern; the
// ORIGINAL runs from a raw call thunk; its results kept, the snapshot restored, the REWRITE runs the same way. Compared:
// every byte of .data/.bss/.idata and the 1 MB arena (a dword where both passes left a pointer into this thread's stack
// counts as equal), the return value, the call logs of every stub, faults. Every byte the original changed must lie in the
// rewrite's footprint (unless it's replay_only). The x87 runs at 24 or 53 bits (alternating rounds).
//
// Then directed_tests: MenuMultiChooseTransport driven down each path by a script (a tab's radio button clicked, a button
// found by its callback and clicked, Enter / Escape / Up / text typed, frames waited for a cancel box to close itself):
// LAN Connect with and without a password, over IPX, Create, Find "host:5", Back, Direct Connect (a socket on the line; no
// socket; the line check failing), Modem Call, Answer an offered call, a device that can't open, the tabs cycled with Up.
// Each is compared as in the rounds, and the original's pass must have taken the path (its return, a stub it reached,
// what it left in the MultiGenesisInfo).
//
// Built with /DVP_MENU_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Multiplayer
// menus"): the rounds keep to the cases the fixes leave alone (the LAN list's selection one of the games listed, the input
// box's sizes holding their texts -- the stubs' texts are short) and every function is compared as above; then
// directed_fix_tests runs each fix on its bad case (the rewrite alone: clean, the promised result) and its boundary case on
// both, bit for bit. VP_FIXTEST=n runs fix group n alone; built with /DVP_MENU_FIXES /DVP_FAITHFUL the same tests run on
// the faithful rewrites, and every group fails (or crashes).
//
// Stubbed (a jump to a logger; state in the arena so both passes see the same): world_menu_options.cpp's toolkit stubs
// (MemAlloc / delete, the logs, the input and the clock, the 2D calls, the fonts and stamps, Xlator::xlate, sprintf, the
// files, atexit, the CRT's fatal paths), the options, and this step's callees outside the menus: the multi library
// (SocketCreateUDP / SocketCreateIPX / SocketCreateLine, CreateSessionMgr and the SessionMgr methods mmulti calls --
// Tick, ServiceRequestBroadcast, GetServiceTable, CanDestroySafely, ~SessionMgr, AsyncServiceRequestByHostname,
// AsyncCancel --, LineEnumerateDevices (its entries' create: a stub handing out fake ILineDevices whose every slot logs),
// LineStatusText, LineDeviceInfo::GetError, LineChecker's constructor / Tick / DoneChecking / LineOk / IsServer),
// Win32Idle, gxTextWidth. The game's own code runs everywhere else: the whole widget toolkit (UIDoDialog, _UIAddItems,
// UIDoInputBox, UIDoOkBox, the widgets, UICustomControl), UIDialogItem's and Xlator's constructors, strchr, atoi.
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
#include <string>
#include <utility>
#include <vector>

#ifndef VP_MENU_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#define MENU_FIXES 0
#else
#define MENU_FIXES 1                // (built with /DVP_MENU_FIXES: the fixes on, and tested -- see directed_fix_tests)
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

#include "../hook/menu_multi.cpp"

using namespace uit;
using namespace mob;
using namespace mmu;

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
enum { OP_NOP, OP_MOVE_W, OP_MOVE_XY, OP_DOWN, OP_UP, OP_RDOWN, OP_RUP, OP_KEY, OP_MOVE_CB, OP_MOVE_VAR };
// OP_MOVE_CB: onto the button calling op[1]; OP_MOVE_VAR: onto the op[2]-th radio button on the variable at op[1]
struct HState {
    uint32_t heap_next;
    int32_t time, frames, frame_limit, time_step;
    int32_t script_n, script_pos;
    int32_t script[64][3];
    int32_t mouse_x, mouse_y;
    int32_t detect_calls, detect_after;
    int32_t grab_fail_at;
    int32_t pal_next, alloc_canvas_next;
    int32_t nfiles, file_pos;
    char files[8][24];
    int32_t file_open_mask;
    uint32_t opt_seed;                      // which options are present, and their values
    // this step's
    uint32_t sock_fail;                     // bit 0 UDP, 1 IPX, 2 a line: the creator returns 0
    int32_t sock_next;
    uint32_t mgr_fail;                      // by call: CreateSessionMgr returns 0
    int32_t mgr_calls;
    int32_t can_destroy_after, can_destroy_calls;   // SessionMgr::CanDestroySafely: yes from this call on
    int32_t async_state;                    // AsyncServiceRequestByHostname leaves it in the block
    int32_t ndev;                           // LineEnumerateDevices' count
    uint8_t dev_flag0[5], dev_in_use[5];
    uint8_t _pad[2];
    uint32_t create_fail;                   // by call: a device's create returns 0
    int32_t create_calls;
    int32_t status[8];                      // ILineDevice::GetStatus's answers, in turn
    int32_t status_calls;
    uint32_t dev_bits;                      // ILineDevice::CanDestroySafely's answers
    int32_t dev_calls;
    uint32_t chk_bits;                      // LineChecker's DoneChecking / LineOk / IsServer answers
    int32_t chk_calls;
    int32_t chk_got, chk_sent, chk_seq;     // what LineChecker's constructor leaves
    uint32_t idle_bits;                     // the harness's own cancel-box idle function
    int32_t idle_calls;
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
    HS->time += HS->time_step;
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
// the centre of the running window's button (or hot key's: none has a place) whose callback is cb
static bool widget_with_cb(uint32_t cb, int32_t* x, int32_t* y) {
    const WidgetWindow* w = UI_GP(WidgetWindow, S_ACTIVE);
    if (!w || !readable(w, sizeof(WidgetWindow)) || w->count <= 0 || w->count > 256) return false;
    for (int32_t i = 0; i < w->count; i++) {
        const Widget* g = w->widgets[i];
        if (!g || !readable(g, sizeof(Widget))) continue;
        const uint32_t vt = (uint32_t)(uintptr_t)g->vtbl;
        uint32_t c = 0;
        if (vt == VT_Button || vt == VT_MenuButton) c = (uint32_t)(uintptr_t)((const Button*)g)->cb;
        else if (vt == VT_BButton) c = (uint32_t)(uintptr_t)((const BButton*)g)->cb;
        if (c != cb) continue;
        *x = w->x + (g->x0 + g->x1) / 2;
        *y = w->y + (g->y0 + g->y1) / 2;
        return true;
    }
    return false;
}
// the centre of the running window's k-th radio button on the variable at var
static bool widget_with_var(uint32_t var, int32_t k, int32_t* x, int32_t* y) {
    const WidgetWindow* w = UI_GP(WidgetWindow, S_ACTIVE);
    if (!w || !readable(w, sizeof(WidgetWindow)) || w->count <= 0 || w->count > 256) return false;
    for (int32_t i = 0; i < w->count; i++) {
        const Widget* g = w->widgets[i];
        if (!g || !readable(g, sizeof(Widget)) || (uint32_t)(uintptr_t)g->vtbl != VT_RadioButton) continue;
        if ((uint32_t)(uintptr_t)((const RadioButton*)g)->var != var || k-- > 0) continue;
        *x = w->x + (g->x0 + g->x1) / 2;
        *y = w->y + (g->y0 + g->y1) / 2;
        return true;
    }
    return false;
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
        case OP_MOVE_VAR: if (!widget_with_var((uint32_t)op[1], op[2], &HS->mouse_x, &HS->mouse_y)) { L('NOVR'); L((uint32_t)op[1]); } t = 6; break;
        case OP_MOVE_CB: if (!widget_with_cb((uint32_t)op[1], &HS->mouse_x, &HS->mouse_y)) { L('NOCB'); L((uint32_t)op[1]); } t = 6; break;
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


// ---- stubs: text, files (world_ui.cpp's) --------------------------------------------------------------------------------------
static uint32_t xl_text(uint32_t key) { return key; }       // an Xlator's text: its key
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
static bool opt_present(uint32_t h) { h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; return (h & 7) != 0; }   // (mixed: the directed scenarios pick keys)
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

// ---- stubs: the multi library, the line devices ---------------------------------------------------------------------------------
// the world's fakes (set up in build_world): two session managers with their service tables, three line devices
struct FakeDev { const void* vtbl; int32_t k; };
static uint8_t* g_mgrs[2];
static uint8_t* g_tables[2];
static FakeDev* g_devs;
static uint32_t fake_sock(uint32_t kind) { return 0x5a000000u + (kind << 16) + (uint32_t)(HS->sock_next++) * 16u; }
static void* __cdecl stub_SocketCreateUDP(uint32_t port) {
    L('SUDP'); L(port & 0xffff);
    return (HS->sock_fail & 1) ? 0 : (void*)(uintptr_t)fake_sock(1);
}
static void* __cdecl stub_SocketCreateIPX(uint32_t port) {
    L('SIPX'); L(port & 0xffff);
    return (HS->sock_fail & 2) ? 0 : (void*)(uintptr_t)fake_sock(2);
}
static void* __cdecl stub_SocketCreateLine(void* dev) {
    L('SCLN'); L(P(dev));
    return (HS->sock_fail & 4) ? 0 : (void*)(uintptr_t)fake_sock(3);
}
static void* __cdecl stub_CreateSessionMgr(uint32_t sock, int32_t a, int32_t b, int32_t c) {
    L('CSMG'); L(sock); L((uint32_t)a); L((uint32_t)b); L((uint32_t)c);
    const int32_t k = HS->mgr_calls++;
    if ((HS->mgr_fail >> (k & 31)) & 1) return 0;
    return g_mgrs[k & 1];
}
static void __fastcall stub_SM_Tick(void* self, int) { L('SMTK'); L(P(self)); }
static void __fastcall stub_SM_ServiceRequestBroadcast(void* self, int, uint32_t type) { L('SMRB'); L(P(self)); L(type); }
static const uint8_t* __fastcall stub_SM_GetServiceTable(void* self, int) {
    L('SMST'); L(P(self));
    return self == g_mgrs[1] ? g_tables[1] : g_tables[0];
}
static uint8_t __fastcall stub_SM_CanDestroySafely(void* self, int) {
    L('SMCD'); L(P(self));
    return HS->can_destroy_calls++ >= HS->can_destroy_after ? 1 : 0;
}
static void __fastcall stub_SM_dtor(void* self, int) { L('SMDT'); L(P(self)); }
static void __fastcall stub_SM_AsyncServiceRequestByHostname(void* self, int, const char* host, uint32_t port, uint32_t type, uint8_t* block) {
    L('SMAH'); L(P(self)); LS(host, 64); L(port & 0xffff); L(type); L(P(block));
    if (readable(block, 0x438)) {
        *(int32_t*)(block + 4) = HS->async_state;
        strcpy((char*)block + 0x418, "Looking up host");
    }
}
static void __fastcall stub_SM_AsyncCancel(void* self, int, void* block) { L('SMAC'); L(P(self)); L(P(block)); }
// the line devices
static void* __cdecl stub_create_device(void* info) {
    L('DCRE'); L(P(info));
    const int32_t k = HS->create_calls++;
    if ((HS->create_fail >> (k & 31)) & 1) return 0;
    return &g_devs[k % 3];
}
static int32_t __cdecl stub_LineEnumerateDevices(uint8_t* infos, int32_t n, uint32_t kind) {
    L('LENU'); L(P(infos)); L((uint32_t)n); L(kind & 0xff);
    const int32_t c = HS->ndev < n ? HS->ndev : n;
    for (int32_t i = 0; i < c; i++) {
        LineDevInfo* d = (LineDevInfo*)(infos + 0x50 * i);
        d->flag0 = HS->dev_flag0[i];
        d->in_use = HS->dev_in_use[i];
        sprintf(d->name, "Device %d (%u)", i, kind & 0xff);
        d->create = &stub_create_device;
        d->id = i;
    }
    return HS->ndev;
}
static const char* g_fx_status;                            // (the fix tests': a status text of their own, when set)
static const char* __cdecl stub_LineStatusText(int32_t st) {
    L('LSTX'); L((uint32_t)st);
    if (g_fx_status) return g_fx_status;
    static const char* t[16] = {"Idle", "Dialing", "Connected", "Busy", "No answer", "No dialtone", "Ringing", "Hung up", "Error",
                                "Proceeding", "Offering", "Unknown", "?12", "?13", "?14", "?15"};
    return t[(uint32_t)st & 15];
}
static const char* __fastcall stub_LDI_GetError(void* self, int) { L('LERR'); L(P(self)); return "The device failed"; }
static void* __fastcall stub_LineChecker_ctor(uint8_t* self, int, void* dev) {
    L('LCHK'); L(P(self)); L(P(dev));
    memset(self, 0, 0x28);
    memcpy(self, &dev, 4);
    *(int32_t*)(self + 0x10) = HS->chk_got;
    *(int32_t*)(self + 0x14) = HS->chk_sent;
    *(int32_t*)(self + 0x18) = HS->chk_seq;
    return self;
}
static uint8_t chk_next() { return (uint8_t)((HS->chk_bits >> (HS->chk_calls++ & 31)) & 1); }
static void __fastcall stub_LineChecker_Tick(uint8_t* self, int) {
    L('LCTK'); L(P(self));
    if (readable(self, 0x28)) *(int32_t*)(self + 0x14) += 1;
}
static uint8_t __fastcall stub_LineChecker_DoneChecking(void* self, int) { L('LCDN'); L(P(self)); return chk_next(); }
static uint8_t __fastcall stub_LineChecker_LineOk(void* self, int) { L('LCOK'); L(P(self)); return chk_next(); }
static uint8_t __fastcall stub_LineChecker_IsServer(void* self, int) { L('LCSV'); L(P(self)); return chk_next(); }
// ILineDevice: every slot logs
static void __fastcall dev_Shutdown(FakeDev* d, int) { L('DSHU'); L((uint32_t)d->k); }
static uint8_t __fastcall dev_CanDestroySafely(FakeDev* d, int) {
    L('DCDS'); L((uint32_t)d->k);
    return (uint8_t)((HS->dev_bits >> (HS->dev_calls++ & 31)) & 1);
}
static void* __fastcall dev_dtor(FakeDev* d, int, uint32_t fl) { L('DDEL'); L((uint32_t)d->k); L(fl); return d; }
static int32_t __fastcall dev_BPS(FakeDev* d, int) { L('DBPS'); L((uint32_t)d->k); return 9600; }
static int32_t __fastcall dev_GetStatus(FakeDev* d, int) {
    L('DSTA'); L((uint32_t)d->k);
    return HS->status[HS->status_calls++ & 7];
}
template <int S> static int32_t __fastcall dev_other(FakeDev* d, int) { L('DOTH'); L(S); L((uint32_t)d->k); return 0; }
static void __fastcall dev_Call(FakeDev* d, int, const char* num) { L('DCAL'); L((uint32_t)d->k); LS(num, 64); }
static void __fastcall dev_Answer(FakeDev* d, int) { L('DANS'); L((uint32_t)d->k); }
static void* g_dev_vtbl[13] = {
    (void*)&dev_Shutdown, (void*)&dev_CanDestroySafely, (void*)&dev_dtor, (void*)&dev_BPS, (void*)&dev_GetStatus,
    (void*)&dev_other<0x14>, (void*)&dev_other<0x18>, (void*)&dev_other<0x1c>, (void*)&dev_other<0x20>, (void*)&dev_Call,
    (void*)&dev_other<0x28>, (void*)&dev_Answer, (void*)&dev_other<0x30>,
};
// the rest
static void __cdecl stub_Win32Idle() { L('IDLE'); }
static int32_t __cdecl stub_gxTextWidth(const char* s) { L('GTXW'); LS(s); return readable(s, 1) ? (int32_t)strlen(s) * 7 : 0; }
// a cancel box's idle function of the harness's own: yes or no, in turn
static uint8_t __cdecl stub_idle() { L('IDLF'); return (uint8_t)((HS->idle_bits >> (HS->idle_calls++ & 31)) & 1); }

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
    NetBrowser* nb;
    LineControl* modem;
    LineControl* direct;
    uint8_t* block;                                 // find_host's AsyncServiceRequestBlock (0x438)
    uint8_t* checker;                               // a LineChecker (0x28)
    char* msgbuf;                                   // a cancel box's message (0x100)
    char* scratch;                                  // fresh blocks for constructors (0x800)
    MultiGenesisInfo* info;
    char* strs;                                     // the arguments' strings (0x400)
    uint32_t* arr;                                  // the input box's arrays (16)
    char* bufs;                                     // its buffers (2 x 0x100)
    int32_t* ints;
    UIDialogItem* items;
    std::vector<Widget*> widgets;
};
static World W;
static std::vector<uint32_t> g_setup_e;              // the ui and menu libraries' $E initialisers (setup only)

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
        if ((!strcmp(c[2], "ui") || !strcmp(c[2], "menu")) && c[5][0] == '$' && c[5][1] == 'E')
            g_setup_e.push_back((uint32_t)strtoul(c[0], 0, 16));
    }
    fclose(f);
}
static const char* k_names[8] = {"Joe's game", "", "LAN party", "x", "Viper Night", "a", "Thirty-one characters, exactly!", "pw"};
static void build_world() {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    HS->time_step = 16;
    HS->opt_seed = 0x1234567;
    HS->ndev = 3;
    UI_G32(S_SCREEN_W) = 640;
    UI_G32(S_SCREEN_H) = 480;
    for (uint32_t a : g_setup_e) call_orig(a, 0, {});
    call_orig(0x004779a0, 0, {});                   // UIBegin
    W.screen = (gxCanvas*)wv(sizeof(gxCanvas));
    W.screen->format = 4; W.screen->pixels = (uint8_t*)0x7b000000; W.screen->w = 640; W.screen->h = 480; W.screen->pitch = 1280;
    W.screen->cx1 = 640; W.screen->cy1 = 480;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    for (int k = 0; k < 2; k++) { g_mgrs[k] = (uint8_t*)wv(0xd8); g_tables[k] = (uint8_t*)wv(0x54 * 12); }
    g_devs = (FakeDev*)wv(sizeof(FakeDev) * 3);
    for (int k = 0; k < 3; k++) { g_devs[k].vtbl = g_dev_vtbl; g_devs[k].k = k; }
    W.block = (uint8_t*)wv(0x438);
    W.checker = (uint8_t*)wv(0x28);
    W.msgbuf = (char*)wv(0x100);
    W.scratch = (char*)wv(0x800);
    W.info = (MultiGenesisInfo*)wv(sizeof(MultiGenesisInfo));
    W.strs = (char*)wv(0x400);
    W.arr = (uint32_t*)wv(16 * 4);
    W.bufs = (char*)wv(0x200);
    W.ints = (int32_t*)wv(16 * 4);
    // the tabs' controls, by their original constructors
    W.nb = (NetBrowser*)wv(sizeof(NetBrowser));
    W.modem = (LineControl*)wv(sizeof(LineControl));
    W.direct = (LineControl*)wv(sizeof(LineControl));
    call_orig(F_NetBrowser_ctor, 1, {U32(W.nb), 0});
    call_orig(F_LineControl_ctor, 1, {U32(W.modem), 0, 1});
    W.modem->vtbl = (const void*)(uintptr_t)VT_ModemLineControl;
    UI_GP(void, S_MLC_GLOBAL) = W.modem;
    strcpy(W.modem->phone, "555-0100");
    call_orig(F_LineControl_ctor, 1, {U32(W.direct), 0, 2});
    W.direct->vtbl = (const void*)(uintptr_t)VT_DirectLineControl;
    UI_GP(void, S_DLC_GLOBAL) = W.direct;
    // a full-screen window, running, and the controls added to it by the game's _UIAddItems
    call_orig(F_WidgetCreateWindow, 0, {U32("main_t.stp"), 0, 0, 640, 480, 2});
    W.win = (WidgetWindow*)(uintptr_t)g_r_eax;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    W.items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 8);
    UICustomControl* ctl[3] = {W.nb, W.direct, W.modem};
    const int32_t pos[3][4] = {{0xf0, 0x96, 0x100, 0xc8}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    for (int k = 0; k < 3; k++) {
        UIDialogItem* it = &W.items[k];
        it->type = 0x17; it->x = pos[k][0]; it->y = pos[k][1]; it->w = pos[k][2]; it->h = pos[k][3]; it->text = "";
        it->data = ctl[k];
    }
    call_orig(F_UIAddItems, 0, {U32(W.win), U32(W.items)});
    for (int i = 0; i < W.win->count; i++) W.widgets.push_back((Widget*)W.win->widgets[i]);
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
static void short_str(char* d, int max) {
    const char* s = k_names[rnd() & 7];
    strncpy(d, s, (size_t)max - 1);
    d[max - 1] = 0;
}
static uint32_t group_mask() { return 1u << irange(0, 15); }
static void random_line(LineControl* lc) {
    lc->count = irange(0, 5);
    lc->sel = chance(95) ? irange(0, lc->count > 0 ? lc->count - 1 : 0) : irange(0, 4);
    lc->device = chance(90) ? (void*)&g_devs[irange(0, 2)] : 0;
    lc->msg = W.msgbuf;
    lc->timer = chance(50) ? 0 : HS->time + irange(-3000, 3000);
    lc->checker = W.checker;
    lc->is_server = (uint8_t)(rnd() & 1);
    lc->grp_controls = group_mask(); lc->grp_none = group_mask(); lc->grp_in_use = group_mask();
    for (int i = 0; i < 5; i++) {
        lc->dev[i].flag0 = (uint8_t)(rnd() & 1);
        lc->dev[i].in_use = (uint8_t)(chance(30) ? 1 : 0);
        lc->dev[i].create = &stub_create_device;
        sprintf(lc->dev[i].name, "Line %d", i);
    }
    short_str(lc->phone, 0x20);
}
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
    UI_GF(S_UI_DT) = range(0.0f, 0.25f);
    UI_G32(S_MOUSE_X) = irange(-10, 650); UI_G32(S_MOUSE_Y) = irange(-10, 490);
    HS->time = (int32_t)(chance(10) ? rnd() : rnd() & 0x7fffff);
    static const int32_t steps[4] = {16, 16, 120, 700};
    HS->time_step = steps[rnd() & 3];
    // the LAN tab
    NetBrowser* nb = W.nb;
    nb->proto = irange(0, 1);
    nb->mgr[0] = chance(85) ? (void*)g_mgrs[0] : 0;
    nb->mgr[1] = chance(70) ? (void*)g_mgrs[1] : 0;
    nb->last_broadcast = HS->time - irange(0, 9000);
    nb->sel = irange(0, 7);
    nb->timer = chance(50) ? 0 : HS->time + irange(-3000, 3000);
    nb->block = W.block;
    short_str(nb->game_name, 0x20);
    short_str(nb->password, 0x10);
    for (int k = 0; k < 2; k++) {
        *(int32_t*)(g_mgrs[k] + 0x24) = chance(15) ? 0 : irange(1, 8);
        for (int i = 0; i < 12; i++) {
            uint8_t* rs = g_tables[k] + 0x54 * i;
            memset(rs, 0, 0x54);
            sprintf((char*)rs, "%s%d", k_names[rnd() & 7], i);
            *(int32_t*)(rs + 0x24) = 0x25 + irange(-1, 1);
            *(int32_t*)(rs + 0x40) = irange(-5, 500);
            rs[0x2c] = (uint8_t)(chance(40) ? 1 : 0);
        }
    }
#if MENU_FIXES
    // the fix build: the list's selection one of the games listed (connect's fix makes any other do nothing: the fixed
    // case, run by directed_fix_tests)
    {
        int32_t n = 8;
        for (int k = 0; k < 2; k++) {
            if (*(int32_t*)(g_mgrs[k] + 0x24) == 0) *(int32_t*)(g_mgrs[k] + 0x24) = 1;
            if (*(int32_t*)(g_mgrs[k] + 0x24) < n) n = *(int32_t*)(g_mgrs[k] + 0x24);
        }
        nb->sel = irange(0, n - 1);
    }
#endif
    *(int32_t*)(W.block + 4) = irange(0, 6);
    strcpy((char*)W.block + 0x418, "Finding...");
    // the line tabs
    random_line(W.modem);
    random_line(W.direct);
    strcpy(W.msgbuf, "status");
    memset(W.checker, 0, 0x28);
    *(int32_t*)(W.checker + 0x10) = irange(0, 20); *(int32_t*)(W.checker + 0x14) = irange(0, 30); *(int32_t*)(W.checker + 0x18) = irange(0, 10);
}
// the statics the rewrites read (after the poison: they're random there)
static void randomize_globals() {
    UI_GP(void, S_NB_GLOBAL) = chance(97) ? (void*)W.nb : 0;
    UI_GP(void, S_LC_GLOBAL) = chance(50) ? (void*)W.modem : (void*)W.direct;
    UI_GP(void, S_MLC_GLOBAL) = W.modem;
    UI_GP(void, S_DLC_GLOBAL) = W.direct;
    static const uint32_t idles[6] = {0, F_ASyncFindHost, F_LC_Checking, F_LC_Connecting, F_LC_Answering, 0};
    const uint32_t ix = rnd() % 6;
    UI_GU32(S_CB_IDLE) = ix == 5 ? U32(&stub_idle) : idles[ix];
    {
        const int a = irange(0, 9);
        UI_GU32(S_GRP_LAN) = 1u << a; UI_GU32(S_GRP_DIRECT) = 1u << (a + 1); UI_GU32(S_GRP_MODEM) = 1u << (a + 2);
        const uint32_t g[3] = {S_GRP_LAN, S_GRP_DIRECT, S_GRP_MODEM};
        UI_GU32(S_TAB) = chance(90) ? UI_GU32(g[rnd() % 3]) : rnd();
    }
}
static void randomize_stubs() {
    // the stubs' world
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->detect_calls = 0;
    HS->detect_after = irange(1, 8);
    HS->grab_fail_at = chance(20) ? irange(1, 6) : -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->nfiles = 0;
    HS->file_open_mask = (int32_t)rnd();
    HS->script_n = HS->script_pos = 0;
    HS->opt_seed = rnd();
    HS->sock_fail = chance(25) ? rnd() & 7 : 0;
    HS->sock_next = 0;
    HS->mgr_fail = chance(20) ? rnd() : 0;
    HS->mgr_calls = 0;
    HS->can_destroy_after = irange(0, 6);
    HS->can_destroy_calls = 0;
    HS->async_state = irange(0, 6);
    HS->ndev = irange(0, 5);
    for (int i = 0; i < 5; i++) { HS->dev_flag0[i] = (uint8_t)(rnd() & 1); HS->dev_in_use[i] = (uint8_t)(chance(30) ? 1 : 0); }
    HS->create_fail = chance(30) ? rnd() : 0;
    HS->create_calls = 0;
    for (int i = 0; i < 8; i++) HS->status[i] = chance(40) ? 2 : irange(0, 11);
    HS->status_calls = 0;
    HS->dev_bits = rnd(); HS->dev_calls = 0;
    HS->chk_bits = rnd(); HS->chk_calls = 0;
    HS->chk_got = irange(0, 20); HS->chk_sent = irange(0, 30); HS->chk_seq = irange(0, 10);
    HS->idle_bits = chance(50) ? rnd() : rnd() & rnd() & rnd(); HS->idle_calls = 0;
}
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x126, 0x128, 0x123, 0x124, 0x125, 0x127, 'h', 'o', 's', 't', ':', '5', '0',
                                    '.', 'Z', 9, 0x141};
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
        {0x004f72e4, 0x004f8400},                               // moptions / mrace strings
        {0x004f8b20, 0x004f9100},                               // mmulti's strings, its CreateSocket table
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
    {0x0057a410, 1, 0x0057a4c0, 0x004f8b60}, {0x0057a410, 2, 0x0057a588, 0x004f8b7c}, {0x0057a410, 4, 0x0057a548, 0x004f8ba4},
    {0x0057a43c, 1, 0x0057a3d8, 0x004f8bc8}, {0x0057a43c, 2, 0x0057a510, 0x004f8bd0},
    {0x0057a45c, 1, 0x0057a440, 0x004f8be8}, {0x0057a45c, 2, 0x0057a500, 0x004f8c04},
    {0x0057a488, 1, 0x0057a490, 0x004f8c40},
    {0x0057a384, 1, 0x0057a3b8, 0x004f8c80},
    {0x0057a390, 1, 0x0057a478, 0x004f8cb8}, {0x0057a390, 2, 0x0057a4f0, 0x004f8cd4},
    {0x0057a5d4, 1, 0x0057a398, 0x004f8d00},
    {0x0057a3f4, 1, 0x0057a5b8, 0x004f8d28}, {0x0057a3f4, 2, 0x0057a308, 0x004f8d48},
    {0x0057a340, 1, 0x0057a5c8, 0x004f8d68}, {0x0057a340, 2, 0x0057a3c8, 0x004f8d80}, {0x0057a340, 4, 0x0057a360, 0x004f8d98},
    {0x0057a340, 8, 0x0057a5d8, 0x004f8db0},
    {0x0057a458, 1, 0x0057a530, 0x004f8ddc},
    {0x0057a568, 1, 0x0057a418, 0x004f8e00}, {0x0057a568, 2, 0x0057a3e8, 0x004f8e20},
    {0x0057a584, 1, 0x0057a4e0, 0x004f8e48}, {0x0057a584, 2, 0x0057a3a8, 0x004f8e74}, {0x0057a584, 4, 0x0057a320, 0x004f8ea4},
    {0x0057a32c, 1, 0x0057a520, 0x004f8ebc}, {0x0057a32c, 2, 0x0057a598, 0x004f8ed4}, {0x0057a32c, 4, 0x0057a378, 0x004f8f00},
    {0x0057a318, 1, 0x0057a460, 0x004f8f24}, {0x0057a318, 2, 0x0057a5a8, 0x004f8f44},
    {0x0057a564, 1, 0x0057a330, 0x004f8f68}, {0x0057a564, 2, 0x0057a578, 0x004f8f9c},
    {0x0057a454, 1, 0x0057a2f8, 0x004f8fc4}, {0x0057a454, 2, 0x0057a400, 0x004f8fd4}, {0x0057a454, 4, 0x0057a558, 0x004f8fec},
    {0x0057a46c, 1, 0x0057a4d0, 0x004f9000},
    {0x0057a5c4, 1, 0x0057a4a0, 0x004f9020}, {0x0057a5c4, 2, 0x0057a428, 0x004f9034}, {0x0057a5c4, 4, 0x0057a2d8, 0x004f9044},
    {0x0057a5c4, 8, 0x0057a350, 0x004f9050}, {0x0057a5c4, 0x10, 0x0057a4b0, 0x004f9060}, {0x0057a5c4, 0x20, 0x0057a2e8, 0x004f906c},
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
            UI_GU32(s.xl + 4) = fresh ? s.key : 0;
            UI_GU32(s.xl + 8) = fresh ? UI_GU32(S_XLATOR_COOKIE) : ~UI_GU32(S_XLATOR_COOKIE);
        }
    }
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static void fill_random(void* p, uint32_t n) { uint8_t* b = (uint8_t*)p; for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)rnd(); }
static uint32_t canvas_arg() {
    if (chance(60) && W.win) return U32(&W.win->canvas);
    return U32(W.screen);
}
static void* self_for(const char* cls) {
    if (!strcmp(cls, "NetBrowser")) return W.nb;
    if (!strcmp(cls, "LineControl")) return chance(50) ? W.modem : W.direct;
    if (!strcmp(cls, "ModemLineControl")) return W.modem;
    if (!strcmp(cls, "DirectLineControl")) return W.direct;
    return 0;
}
static uint32_t str_arg(int k) {                           // a short string in the arena
    char* s = W.strs + 0x40 * (k & 15);
    short_str(s, 0x20);
    return U32(s);
}
// the arguments for f (words: ecx, edx first for a __thiscall); false: skip this round
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
    fill_random(W.scratch, 0x800);
#define IS(n) (!strcmp(nm, n))
    if (cls[0]) {
        if (!strcmp(meth, cls)) {                            // a constructor: a fresh random block
            w[0] = U32(W.scratch);
            if (!strcmp(cls, "LineControl")) a[0] = chance(80) ? (uint32_t)irange(1, 2) : rnd();
            return true;
        }
        if (!f.fast) {                                       // a static: the callbacks' int, or nothing
            a[0] = rnd();
            return true;
        }
        void* self = self_for(cls);
        if (!self) return false;
        w[0] = U32(self);
        if (strstr(meth, "deleting destructor")) a[0] = rnd() & 3;
        else if (!strcmp(meth, "Draw")) a[0] = canvas_arg();
        else if (!strcmp(meth, "Callback")) { a[0] = (uint32_t)(chance(85) ? irange(0, 2) : irange(-1, 5)); a[1] = chance(50) ? 0 : U32(&W.ints[0]); }
        else if (IS("NetBrowser::find_host")) {
            a[0] = str_arg(0);
            a[1] = chance(50) ? 0x7d1 : (uint32_t)(int32_t)(int16_t)rnd();
        }
        return true;
    }
    if (IS("MyDoInputBoxN")) {
        a[0] = str_arg(1);
        W.arr[0] = str_arg(2); W.arr[1] = str_arg(3);           // labels
        char* b0 = W.bufs;
        char* b1 = W.bufs + 0x100;
        short_str(b0, 0x20); short_str(b1, 0x10);
        W.arr[2] = U32(b0); W.arr[3] = U32(b1);                 // buffers
        W.arr[4] = (uint32_t)(chance(80) ? 0x20 : irange(0, 0x40)); W.arr[5] = (uint32_t)(chance(80) ? 0x10 : irange(0, 0x40));
#if MENU_FIXES
        // the fix build: each size holds its text (MyDoInputBoxN's fix holds the copy back to it: the fixed case is a text
        // longer than its size, run by directed_fix_tests)
        W.arr[4] = (uint32_t)(chance(80) ? 0x20 : irange(0x20, 0x40)); W.arr[5] = (uint32_t)(chance(80) ? 0x10 : irange(0x10, 0x40));
#endif
        a[1] = U32(&W.arr[0]); a[2] = U32(&W.arr[2]); a[3] = U32(&W.arr[4]); a[4] = 2;
    } else if (IS("MyDoCancelBox")) {
        a[0] = str_arg(4); a[1] = str_arg(5);
        static const uint32_t idles[5] = {0, F_ASyncFindHost, F_LC_Checking, F_LC_Connecting, F_LC_Answering};
        const uint32_t k = rnd() % 6;
        a[2] = k == 5 ? U32(&stub_idle) : idles[k];
    } else if (IS("cb_idle_func")) a[0] = U32(&W.ints[1]);
    else if (IS("CreateSocket")) a[0] = (uint32_t)irange(0, 1);
    else if (IS("MenuMultiChooseTransport")) { fill_random(W.info, sizeof(MultiGenesisInfo)); a[0] = U32(W.info); }
    else a[0] = rnd();                                          // the tab callbacks
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


// ---- directed scenarios: MenuMultiChooseTransport through every tab, button and outcome ----------------------------------------
// The random rounds rarely get a scripted click through to the end of a path (the Create, Connect, Call and Answer buttons
// sit in groups shown only on their tab; a line call takes two cancel boxes). Here each path is driven by a script -- the
// tab keys (Down 0x128 / Up 0x126), Enter, Escape, text, a click on the button with a given callback -- from a world
// where the path can succeed (the clock stepping 0.7 s, the devices connected, the checker's verdicts set), and the
// original is compared with the rewrite exactly as in the rounds. Each scenario also checks that the original's pass took
// the path it's meant to (its return, a stub it reached, what it left in the MultiGenesisInfo), so the comparison covers it.
static int g_dt_bad, g_dt_n;
static int g_sn;
static void sc(int32_t op, int32_t a = 0) {
    if (g_sn >= 62) return;
    HS->script[g_sn][0] = op; HS->script[g_sn][1] = a; HS->script[g_sn][2] = 0;
    HS->script_n = ++g_sn;
}
static void sc_key(int32_t k) { sc(OP_NOP); sc(OP_KEY, k); }
static void sc_wait(int n) { while (n-- > 0) sc(OP_NOP); }       // frames for a cancel box to close by itself
static void sc_click(uint32_t cb) { sc(OP_NOP); sc(OP_MOVE_CB, (int32_t)cb); sc(OP_DOWN); sc(OP_UP); }
static void sc_tab(int32_t k) {                                 // a tab's radio button: 0 Lan, 1 Direct, 2 Modem
    sc(OP_NOP);
    if (g_sn < 62) { HS->script[g_sn][0] = OP_MOVE_VAR; HS->script[g_sn][1] = (int32_t)S_TAB; HS->script[g_sn][2] = k; HS->script_n = ++g_sn; }
    sc(OP_DOWN); sc(OP_UP);
}
static int32_t opt_int(const char* sec, const char* key) {      // what the OptionsGetI stub gives a present key
    const uint32_t h = opt_h(sec, key);
    return (int32_t)((h >> 3) % 7) - 1 + (((h >> 8) & 15) == 0 ? 0x1f : 0);
}
static void dt_world(uint32_t seed) {
    g_rng = seed * 2654435761u | 1;
    mem_load(g_pristine);
    randomize_world();
    randomize_globals();
    randomize_stubs();
    randomize_statics();
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_G8(S_EXIT) = 0;
    HS->time_step = 700;
    HS->frame_limit = 400;
    HS->sock_fail = 0; HS->mgr_fail = 0; HS->can_destroy_after = 0; HS->ndev = 2; HS->create_fail = 0; HS->async_state = 1;
    for (int i = 0; i < 5; i++) { HS->dev_flag0[i] = 1; HS->dev_in_use[i] = 0; }
    for (int i = 0; i < 8; i++) HS->status[i] = 2;
    HS->chk_bits = ~0u;
    for (int k = 0; k < 2; k++) {
        *(int32_t*)(g_mgrs[k] + 0x24) = 3;
        for (int i = 0; i < 12; i++) g_tables[k][0x54 * i + 0x2c] = 0;
    }
    while (opt_present(opt_h("MULTI", "main_tab")) || opt_present(opt_h("MULTI", "net_protocol"))) HS->opt_seed++;
    g_sn = 0;
    HS->script_n = HS->script_pos = 0;
}
static bool logged(const CallLog& lg, uint32_t tag) {
    for (uint32_t i = 0; i < lg.n && i < LOG_MAX; i++)
        if (lg.w[i] == tag) return true;
    return false;
}
// the original, then the rewrite, from the same state: the same? (the original's log and MultiGenesisInfo kept for the checks)
static MultiGenesisInfo g_dt_info;
static bool dt_pair(const Ent& f, const uint32_t* w, Result* ro_out) {
    uint32_t words[72] = {};
    memcpy(words, w, 4 * (size_t)(f.nstack + (f.fast ? 2 : 0)));
    g_pc = _PC_53;
    mem_save(g_snap);
    const Result ro = run(f, false, words);
    mem_save(g_after);
    memcpy(&g_log_orig, &g_log, sizeof(uint32_t) * (1 + (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)));
    memcpy(&g_dt_info, W.info, sizeof g_dt_info);
    mem_load(g_snap);
    const Result rn = run(f, true, words);
    *ro_out = ro;
    bool same = ro.fault == rn.fault && ro.code == rn.code && ro.ret == rn.ret && ro.pops == rn.pops && ro.top == rn.top &&
                !memcmp(ro.regs, rn.regs, sizeof ro.regs);
    const uint32_t where = mem_diff(g_after);
    same &= where == 0;
    same &= g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
    if (!same) {
        printf("    differs: fault %d/%d, return %08x / %08x, memory at %08x, logs %u / %u words\n", ro.fault, rn.fault, ro.ret, rn.ret,
               where, g_log_orig.n, g_log.n);
        for (uint32_t i = 0; i < g_log.n && i < g_log_orig.n && i < LOG_MAX; i++)
            if (g_log.w[i] != g_log_orig.w[i]) { printf("    the logs differ at word %u: %08x / %08x\n", i, g_log_orig.w[i], g_log.w[i]); break; }
    }
    return same;
}
static void dt_check(bool ok, const char* scen, const char* what) {
    g_dt_n++;
    if (ok) return;
    g_dt_bad++;
    printf("  SCENARIO FAILED: %s: %s\n", scen, what);
}
static int directed_tests() {
    const Ent* f = 0;
    for (int i = 0; i < g_nfns; i++)
        if (!strcmp(g_fns[i].name, "MenuMultiChooseTransport")) f = &g_fns[i];
    if (!f) { printf("  MenuMultiChooseTransport isn't listed\n"); return 1; }
    enum { LAN_CONNECT, LAN_PASSWORD, IPX_CONNECT, CREATE, BACK, DIRECT, DIRECT_NOSOCK, DIRECT_BADLINE, MODEM_CALL, MODEM_ANSWER,
           MODEM_NODEV, FIND, PGUP, N };
    static const char* names[N] = {"LAN: Connect (Enter), no password", "LAN: Connect, a password asked", "LAN: IPX, Connect",
                                   "LAN: Create, a name typed", "Back (Escape)", "Direct: Connect",
                                   "Direct: Connect, no socket on the line", "Direct: Connect, the line check fails", "Modem: Call",
                                   "Modem: Answer an offered call", "Modem: Call, the device can't open", "LAN: Find a host:port",
                                   "Up (0x126) round the tabs"};
    for (int si = 0; si < N; si++)
        for (int rep = 0; rep < 4; rep++) {
            const char* nm = names[si];
            dt_world(0x7000 + 16 * si + rep);
            uint32_t want_tag = 0, want_ret = 1;
            int want_named = -1, want_server = -1;
            switch (si) {
            case LAN_CONNECT: sc_tab(0); sc_click(F_NB_Connect); want_tag = 'SUDP'; want_named = 0; want_server = 0; break;
            case LAN_PASSWORD:
                for (int k = 0; k < 2; k++) for (int i = 0; i < 12; i++) g_tables[k][0x54 * i + 0x2c] = 1;
                sc_tab(0); sc_click(F_NB_Connect); sc_key('p'); sc_key('w'); sc_key(0xd);
                want_tag = 'SUDP'; want_named = 0; want_server = 0;
                break;
            case IPX_CONNECT:
                while (opt_present(opt_h("MULTI", "main_tab")) || !opt_present(opt_h("MULTI", "net_protocol")) ||
                       opt_int("MULTI", "net_protocol") != 1)
                    HS->opt_seed++;
                sc_tab(0); sc_click(F_NB_Connect); want_tag = 'SIPX'; want_named = 0; want_server = 0;
                break;
            case CREATE: sc_tab(0); sc_click(F_NewServer); sc_key('g'); sc_key(0xd); want_tag = 'SUDP'; want_named = 1; want_server = 1; break;
            case BACK: sc_key(0x1b); want_ret = 0; want_tag = 'OSS '; break;
            case DIRECT: sc_tab(1); sc_click(F_DLC_Connect); want_tag = 'SCLN'; want_named = 1; break;
            case DIRECT_NOSOCK: HS->sock_fail = 4; sc_tab(1); sc_click(F_DLC_Connect); want_tag = 'LOGR'; want_ret = 0; break;
            case DIRECT_BADLINE:
                HS->chk_bits = 1;                               // done, then not ok
                sc_tab(1); sc_click(F_DLC_Connect); sc_wait(16); sc_key(0x1b);
                want_tag = 'DSHU'; want_ret = 0;
                break;
            case MODEM_CALL: sc_tab(2); sc_click(F_MLC_Call); want_tag = 'DCAL'; want_named = 1; break;
            case MODEM_ANSWER:
                HS->status[0] = 10; HS->status[1] = 10;
                sc_tab(2); sc_click(F_MLC_Answer);
                want_tag = 'DANS'; want_named = 1;
                break;
            case MODEM_NODEV:
                HS->create_fail = ~0u;
                sc_tab(2); sc_click(F_MLC_Call); sc_key(0xd); sc_key(0x1b);
                want_tag = 'LERR'; want_ret = 0;
                break;
            case FIND:
                sc_tab(0); sc_click(F_FindHost);
                for (const char* t = "host:5"; *t; t++) sc_key(*t);
                sc_key(0xd); sc_wait(16); sc_key(0x1b);
                want_tag = 'SMAH'; want_ret = 0;
                break;
            case PGUP: sc_tab(1); for (int k = 0; k < 4; k++) sc_key(0x126); sc_key(0x1b); want_tag = 'OSI '; want_ret = 0; break;
            }
            if (getenv("VP_TRACE")) printf("  scenario %s (case %d)\n", nm, rep);
            fill_random(W.info, sizeof(MultiGenesisInfo));
            const uint32_t w[1] = {U32(W.info)};
            Result ro;
            const bool same = dt_pair(*f, w, &ro);
            if (getenv("VP_DUMP") && atoi(getenv("VP_DUMP")) == si && rep == 0) {     // the original's log, its tags as text
                for (uint32_t i = 0; i < g_log_orig.n && i < LOG_MAX; i++) {
                    const uint32_t x = g_log_orig.w[i];
                    bool tag = true;
                    for (int b = 0; b < 4; b++) { const uint8_t c = (uint8_t)(x >> (8 * b)); if (!(c == ' ' || (c >= 'A' && c <= 'Z'))) tag = false; }
                    if (tag) printf(" %c%c%c%c", (char)(x >> 24), (char)(x >> 16), (char)(x >> 8), (char)x);
                    else printf(" %x", x);
                }
                printf("\n");
            }
            char m[200];
            sprintf(m, "the rewrite as the original (case %d)", rep);
            dt_check(same, nm, m);
            sprintf(m, "the original's pass returned %u (want %u) without a fault, and reached %c%c%c%c (case %d)", ro.ret, want_ret,
                    (char)(want_tag >> 24), (char)(want_tag >> 16), (char)(want_tag >> 8), (char)want_tag, rep);
            dt_check(!ro.fault && ro.ret == want_ret && logged(g_log_orig, want_tag), nm, m);
            if (want_named >= 0) {
                sprintf(m, "the info: named %d (want %d), server %d (want %d), a socket (case %d)", g_dt_info.named, want_named,
                        g_dt_info.is_server, want_server, rep);
                dt_check(g_dt_info.named == want_named && (want_server < 0 || g_dt_info.is_server == want_server) && g_dt_info.sock, nm, m);
            }
        }
    mem_load(g_pristine);
    printf("directed scenarios (MenuMultiChooseTransport): %s -- %d checks, %d failed\n", g_dt_bad ? "FAILED" : "all passed", g_dt_n, g_dt_bad);
    return g_dt_bad;
}

#if MENU_FIXES
// ---- the fixes, each on its bad case (directed_fix_tests) -----------------------------------------------------------------
// Built with /DVP_MENU_FIXES. Each fix's bad case runs on the rewrite alone (the original would overrun its frame there, or
// read through a stray pointer -- this program's stack among what it would take), from a directed world (dt_world): it must
// return cleanly (no fault, the bytes popped, ebx / esi / edi / ebp kept) and give what the fix promises (a text cut to its
// buffer, the bytes after it untouched, no call made). Each fix's boundary case (the longest input that fits, or the last
// one in range) runs on the original and the rewrite from the same state and must give the same memory, call logs and
// result. Callees whose output is the evidence are caught for the test's length (MyDoCancelBox, UIDoOkBox, UIDoDialog:
// their arguments recorded, a set answer returned) in both passes alike.
static int g_fx_bad, g_fx_n, g_fx_same_n;
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
static void fx_check(bool ok, const char* what, const Result* r = 0) {
    g_fx_n++;
    if (ok) return;
    g_fx_bad++;
    printf("  FIX TEST FAILED: %s", what);
    if (r) printf(" (fault %d %08x at %08x, popped %u, return %08x, ebx esi edi ebp %08x %08x %08x %08x)", r->fault, r->code, r->eip,
                  r->pops, r->ret, r->regs[0], r->regs[1], r->regs[2], r->regs[3]);
    printf("\n");
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
    if (!same) printf("    (memory at %08x, return %08x / %08x, logs %u / %u words)\n", where, ro.ret, rn.ret, g_fx_log.n, g_log.n);
    fx_check(same, m, &ro);
}
// a function-local Xlator constructed (its guard's bit set) with the text given, fresh
static void fx_xl(uint32_t guard, uint8_t bit, uint32_t xl, const char* text) {
    UI_G8(guard) = (uint8_t)(UI_G8(guard) | bit);
    UI_GU32(xl + 4) = U32(text);
    UI_GU32(xl + 8) = UI_GU32(S_XLATOR_COOKIE);
}
// a string of n c's and then tail (in one of eight buffers of this program's)
static char g_fx_s[8][0x400];
static const char* mk(int k, char c, int n, const char* tail = "") {
    memset(g_fx_s[k], c, (size_t)n);
    strcpy(g_fx_s[k] + n, tail);
    return g_fx_s[k];
}
// what the game's sprintf would print, its first `keep` characters (this program's sprintf: the same for %s, %d, %-*s)
static std::string fx_expect(uint32_t keep, const char* fmt, ...) {
    static char t[0x2000];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(t, fmt, ap);
    va_end(ap);
    if (strlen(t) > keep) t[keep] = 0;
    return t;
}
// a callee caught for a test: a jump to the harness's own, the original's bytes put back after
struct FxPatch {
    uint32_t at;
    uint8_t b[5];
    FxPatch(uint32_t a, void* to) : at(a) { memcpy(b, (const void*)(uintptr_t)a, 5); patch_jmp(a, to); }
    ~FxPatch() { memcpy((void*)(uintptr_t)at, b, 5); }
};
static char g_fx_title[0x400], g_fx_msg[0x400], g_fx_version[0x400];
static int32_t g_fx_ret;
static int32_t __cdecl fx_cancel_box(const char* title, const char* msg, void*) {
    L('FXCB');
    strncpy(g_fx_title, title, 0x3ff);
    strncpy(g_fx_msg, msg, 0x3ff);
    return g_fx_ret;
}
static void __cdecl fx_ok_box(const char* title, const char* msg) {
    L('FXOK');
    strncpy(g_fx_title, title, 0x3ff);
    strncpy(g_fx_msg, msg, 0x3ff);
}
// UIDoDialog for MenuMultiChooseTransport: the version text (item 0x3dc - 0x94 of the list, its text) recorded, an answer
static int32_t __cdecl fx_dialog(const uint32_t* d, int32_t, int32_t, int32_t, int32_t, int32_t) {
    L('FXDL');
    strncpy(g_fx_version, *(const char* const*)((const uint8_t*)(uintptr_t)d[3] + 0x348 + 0x18), 0x3ff);
    return g_fx_ret;
}
static char g_fx_b0[0x400], g_fx_b1[0x100];

static int directed_fix_tests() {
    char m[256];
    // VP_FIXTEST=n: group n alone (to watch one fix's test on a build without the fixes: /DVP_MENU_FIXES /DVP_FAITHFUL)
    const char* only_fx = getenv("VP_FIXTEST");
    auto fx_on = [&](int g) { return !only_fx || atoi(only_fx) == g; };
    // ---- MyDoInputBoxN: a caller's text that doesn't end inside 255 characters; Ok (Enter) copies back to the size --------
    if (fx_on(1)) {
        const Ent& f = fx_fn("MyDoInputBoxN");
        for (int k = 0; k < 2; k++) {
            dt_world(0x9100 + k);
            memset(g_fx_b0, 'a', 0x300); g_fx_b0[0x300] = 0;
            strcpy(g_fx_b1, "pw");
            if (k) { strcpy(g_fx_b1, g_fx_b0); strcpy(g_fx_b0, "name"); }
            W.arr[0] = str_arg(2); W.arr[1] = str_arg(3); W.arr[2] = U32(g_fx_b0); W.arr[3] = U32(g_fx_b1);
            W.arr[4] = 0x20; W.arr[5] = 0x10;
            g_sn = 0; HS->script_n = HS->script_pos = 0;
            sc_key(0xd);
            const Result r = fx_run(f, true, {str_arg(1), U32(&W.arr[0]), U32(&W.arr[2]), U32(&W.arr[4]), 2});
            sprintf(m, "MyDoInputBoxN, a %d-character text in field %d: a clean return, Ok", 0x300, k);
            fx_check(fx_clean(f, r) && r.ret == 1, m, &r);
            char* const lb = k ? g_fx_b1 : g_fx_b0;
            const uint32_t size = k ? 0x10 : 0x20;
            bool tail = true;
            for (uint32_t i = size; i < 0x300; i++) tail &= lb[i] == 'a';
            sprintf(m, "MyDoInputBoxN, field %d: copied back held to its %u bytes, the rest of the caller's buffer untouched", k, size);
            fx_check(strlen(lb) == size - 1 && tail && !strcmp(k ? g_fx_b0 : g_fx_b1, k ? "name" : "pw"), m);
        }
        dt_world(0x9102);
        memset(g_fx_b0, 'b', 0x1f); g_fx_b0[0x1f] = 0;
        memset(g_fx_b1, 'c', 0xf); g_fx_b1[0xf] = 0;
        W.arr[0] = str_arg(2); W.arr[1] = str_arg(3); W.arr[2] = U32(g_fx_b0); W.arr[3] = U32(g_fx_b1);
        W.arr[4] = 0x20; W.arr[5] = 0x10;
        g_sn = 0; HS->script_n = HS->script_pos = 0;
        sc_key(0xd);
        fx_same(f, {str_arg(1), U32(&W.arr[0]), U32(&W.arr[2]), U32(&W.arr[4]), 2}, "MyDoInputBoxN, texts of 31 and 15 (the sizes' most)");
    }
    // ---- find_host: a long translation in the title ("%s %s (%d)", "%s %s") ------------------------------------------------
    if (fx_on(2)) {
        const Ent& f = fx_fn("NetBrowser::find_host");
        FxPatch cb(F_MyDoCancelBox, (void*)&fx_cancel_box);
        const char* host = "host.example.com";
        for (int k = 0; k < 2; k++) {
            dt_world(0x9200 + k);
            g_fx_ret = 0;
            fx_xl(0x0057a488, 1, 0x0057a490, mk(0, 'T', 200));
            strcpy(W.strs, host);
            const uint32_t port = k ? 0x7d1 : 5;
            g_fx_title[0] = 0;
            const Result r = fx_run(f, true, {U32(W.nb), 0, U32(W.strs), port});
            const std::string want = k ? fx_expect(63, "%s %s", g_fx_s[0], host) : fx_expect(63, "%s %s (%d)", g_fx_s[0], host, 5);
            sprintf(m, "find_host, a 200-character translation (port %u): clean, the title its first 63 characters", port);
            fx_check(fx_clean(f, r) && want == g_fx_title, m, &r);
        }
        dt_world(0x9202);
        g_fx_ret = 0;
        fx_xl(0x0057a488, 1, 0x0057a490, mk(0, 'T', 63 - 1 - 16 - 2 - 1 - 1));
        strcpy(W.strs, host);
        fx_same(f, {U32(W.nb), 0, U32(W.strs), 5}, "find_host, a title of 63 characters");
    }
    // ---- NetBrowser::Update: a game of another version, its name not ending in its 32 bytes, a long translation ----------
    if (fx_on(3)) {
        const Ent& f = fx_fn("NetBrowser::Update");
        auto setup = [&](uint32_t seed, int nl, int tl) {
            dt_world(seed);
            W.nb->proto = 0;
            W.nb->mgr[0] = g_mgrs[0];
            *(int32_t*)(g_mgrs[0] + 0x24) = 1;
            uint8_t* rs = g_tables[0];
            memset(rs, 0, 0x54);
            memset(rs, 'N', (size_t)(nl < 0x24 ? nl : 0x24));
            *(int32_t*)(rs + 0x24) = 0x20;                              // older: "OlderVersion"
            fx_xl(0x0057a390, 2, 0x0057a4f0, mk(1, 'O', tl));
            W.nb->list.count = 0;
        };
        setup(0x9300, 0x23, 50);                                         // 35 characters: through the record's type field
        const Result r = fx_run(f, true, {U32(W.nb), 0});
        const std::string want = fx_expect(63, "%-*s %s", 0x20, mk(2, 'N', 0x20), g_fx_s[1]);
        fx_check(fx_clean(f, r) && W.nb->list.count == 1 && want == W.nb->list.entries[0],
                 "NetBrowser::Update, a 35-character name and a 50-character translation: clean, the line its name cut to 32, 63 kept", &r);
        setup(0x9301, 3, 30);
        fx_same(f, {U32(W.nb), 0}, "NetBrowser::Update, a line of 63 characters");
    }
    // ---- place_call / answer: long translations in the titles, a long status ------------------------------------------------
    if (fx_on(4)) {
        FxPatch cb(F_MyDoCancelBox, (void*)&fx_cancel_box);
        FxPatch ok(F_UIDoOkBox, (void*)&fx_ok_box);
        const Ent& pc = fx_fn("LineControl::place_call");
        const Ent& an = fx_fn("LineControl::answer");
        auto setup = [&](uint32_t seed, bool fail) {
            dt_world(seed);
            g_fx_ret = 0;
            LineControl* lc = W.modem;
            lc->sel = 0; lc->count = 2;
            lc->dev[0].create = &stub_create_device; lc->dev[0].flag0 = 1;
            strcpy(lc->phone, mk(3, '5', 31));
            HS->create_fail = fail ? ~0u : 0u; HS->create_calls = 0;
            for (int i = 0; i < 8; i++) HS->status[i] = 2;
            g_fx_title[0] = g_fx_msg[0] = 0;
        };
        setup(0x9400, false);
        fx_xl(0x0057a32c, 1, 0x0057a520, mk(0, 'C', 50));
        g_fx_status = mk(4, 'S', 100);
        Result r = fx_run(pc, true, {U32(W.modem), 0});
        fx_check(fx_clean(pc, r) && r.ret == 1 && fx_expect(63, "%s %s...", g_fx_s[0], g_fx_s[3]) == g_fx_title &&
                 fx_expect(63, "%s", g_fx_s[4]) == g_fx_msg,
                 "place_call, a 50-character translation and a 100-character status: clean, title and message 63 characters", &r);
        setup(0x9401, false);
        W.modem->dev[0].flag0 = 0;
        fx_xl(0x0057a32c, 2, 0x0057a598, mk(0, 'D', 100));
        r = fx_run(pc, true, {U32(W.modem), 0});
        fx_check(fx_clean(pc, r) && fx_expect(63, "%s...", g_fx_s[0]) == g_fx_title, "place_call (a COM port), a long translation: the title 63", &r);
        setup(0x9402, true);
        fx_xl(0x0057a32c, 4, 0x0057a378, mk(0, 'X', 100));
        r = fx_run(pc, true, {U32(W.modem), 0});
        fx_check(fx_clean(pc, r) && r.ret == 0 && fx_expect(63, "%s", g_fx_s[0]) == g_fx_title,
                 "place_call, the device can't open, a long translation: the box's title 63", &r);
        setup(0x9403, false);
        fx_xl(0x0057a318, 1, 0x0057a460, mk(0, 'W', 100));
        r = fx_run(an, true, {U32(W.modem), 0});
        fx_check(fx_clean(an, r) && r.ret == 1 && fx_expect(63, "%s", g_fx_s[0]) == g_fx_title && fx_expect(63, "%s", g_fx_s[4]) == g_fx_msg,
                 "answer, a long translation and status: clean, title and message 63 characters", &r);
        setup(0x9404, true);
        fx_xl(0x0057a318, 2, 0x0057a5a8, mk(0, 'Y', 100));
        r = fx_run(an, true, {U32(W.modem), 0});
        fx_check(fx_clean(an, r) && r.ret == 0 && fx_expect(63, "%s", g_fx_s[0]) == g_fx_title,
                 "answer, the device can't open, a long translation: the box's title 63", &r);
        // the boundaries: a title and a status of 63
        setup(0x9405, false);
        fx_xl(0x0057a32c, 1, 0x0057a520, mk(0, 'C', 63 - 1 - 31 - 3));
        g_fx_status = mk(4, 'S', 63);
        fx_same(pc, {U32(W.modem), 0}, "place_call, a title and a status of 63 characters");
        setup(0x9406, false);
        fx_xl(0x0057a318, 1, 0x0057a460, mk(0, 'W', 63));
        fx_same(an, {U32(W.modem), 0}, "answer, a title and a status of 63 characters");
        g_fx_status = 0;
    }
    // ---- connecting / answering: a long status into the box's message ---------------------------------------------------------
    if (fx_on(5)) {
        for (int k = 0; k < 2; k++) {
            const Ent& f = fx_fn(k ? "LineControl::answering" : "LineControl::connecting");
            dt_world(0x9500 + k);
            LineControl* lc = W.direct;
            lc->timer = 0; lc->device = &g_devs[0]; lc->msg = W.msgbuf;
            memset(W.msgbuf, 'z', 0x100);
            g_fx_status = mk(4, 'S', 200);
            const Result r = fx_run(f, true, {U32(lc), 0});
            bool tail = true;
            for (int i = 0x40; i < 0x100; i++) tail &= W.msgbuf[i] == 'z';
            sprintf(m, "%s, a 200-character status: clean, the message 63 characters, nothing past its 0x40 bytes", f.name);
            fx_check(fx_clean(f, r) && strlen(W.msgbuf) == 63 && tail, m, &r);
            dt_world(0x9502 + k);
            lc = W.direct;
            lc->timer = 0; lc->device = &g_devs[0]; lc->msg = W.msgbuf;
            g_fx_status = mk(4, 'S', 63);
            sprintf(m, "%s, a status of 63 characters", f.name);
            fx_same(f, {U32(lc), 0}, m);
        }
        g_fx_status = 0;
    }
    // ---- update_check_msg / checking: long verdicts, verdicts with conversions ------------------------------------------------
    if (fx_on(6)) {
        const Ent& um = fx_fn("LineControl::update_check_msg");
        const Ent& ck = fx_fn("LineControl::checking");
        auto setup = [&](uint32_t seed, int32_t status) {
            dt_world(seed);
            LineControl* lc = W.direct;
            lc->timer = 0; lc->device = &g_devs[0]; lc->msg = W.msgbuf; lc->checker = W.checker;
            memset(W.checker, 0, 0x28);
            memset(W.msgbuf, 'z', 0x100);
            for (int i = 0; i < 8; i++) HS->status[i] = status;
            HS->chk_bits = ~0u; HS->chk_calls = 0;                          // done, the line ok
        };
        auto tail_ok = [&]() { bool t = true; for (int i = 0x40; i < 0x100; i++) t &= W.msgbuf[i] == 'z'; return t; };
        setup(0x9600, 2);
        fx_xl(0x0057a568, 1, 0x0057a418, mk(0, 'U', 200));
        Result r = fx_run(um, true, {U32(W.direct), 0});
        fx_check(fx_clean(um, r) && strlen(W.msgbuf) == 63 && tail_ok(), "update_check_msg, a 200-character verdict: clean, 63 kept", &r);
        static const char* const texts[4] = {"Line %s is %n ok %d", "%*d", "100%% fine but %s", 0};
        for (int k = 0; k < 4; k++) {
            setup(0x9610 + k, 2);
            const char* t = texts[k] ? texts[k] : mk(1, 'K', 100);
            fx_xl(0x0057a584, 1, 0x0057a4e0, t);
            r = fx_run(ck, true, {U32(W.direct), 0});
            sprintf(m, "checking, the verdict \"%.20s\"%s: clean, shown as written (63 at most)", t, texts[k] ? "" : "...");
            fx_check(fx_clean(ck, r) && fx_expect(63, "%s", t) == W.msgbuf && tail_ok(), m, &r);
        }
        setup(0x9620, 0);
        fx_xl(0x0057a584, 4, 0x0057a320, "%s lost");
        r = fx_run(ck, true, {U32(W.direct), 0});
        fx_check(fx_clean(ck, r) && !strcmp(W.msgbuf, "%s lost"), "checking, the connection lost, \"%s lost\": shown as written", &r);
        // the boundaries: a verdict of 63, one with "%%" (the format as before: '%')
        setup(0x9630, 2);
        fx_xl(0x0057a568, 1, 0x0057a418, mk(0, 'U', 63));
        fx_same(um, {U32(W.direct), 0}, "update_check_msg, a verdict of 63 characters");
        setup(0x9631, 2);
        fx_xl(0x0057a584, 1, 0x0057a4e0, "Line 100%% ok");
        fx_same(ck, {U32(W.direct), 0}, "checking, the verdict \"Line 100%% ok\"");
        setup(0x9632, 2);
        fx_xl(0x0057a584, 1, 0x0057a4e0, mk(1, 'K', 63));
        fx_same(ck, {U32(W.direct), 0}, "checking, a verdict of 63 characters");
    }
    // ---- CreateSocket: a protocol outside 0..1 ------------------------------------------------------------------------------
    if (fx_on(7)) {
        const Ent& f = fx_fn("CreateSocket");
        static const int32_t bad[3] = {2, -1, 0x7fffffff};
        for (int32_t p : bad) {
            dt_world(0x9700);
            const Result r = fx_run(f, true, {(uint32_t)p});
            sprintf(m, "CreateSocket(%d): clean, no socket, nothing called", p);
            fx_check(fx_clean(f, r) && r.ret == 0 && g_log.n == 0, m, &r);
        }
        for (int32_t p = 0; p < 2; p++) {
            dt_world(0x9701 + p);
            sprintf(m, "CreateSocket(%d)", p);
            fx_same(f, {(uint32_t)p}, m);
        }
    }
    // ---- connect: a selection that isn't one of the games listed ---------------------------------------------------------------
    if (fx_on(8)) {
        const Ent& f = fx_fn("NetBrowser::connect");
        static const int32_t bad[3] = {3, -1, 9};
        for (int32_t sel : bad) {
            dt_world(0x9800);
            W.nb->proto = 0; W.nb->mgr[0] = g_mgrs[0];
            *(int32_t*)(g_mgrs[0] + 0x24) = 3;
            W.nb->sel = sel;
            const Result r = fx_run(f, true, {U32(W.nb), 0});
            sprintf(m, "connect, selection %d of 3 games: clean, 0 (nothing joined), nothing called", sel);
            fx_check(fx_clean(f, r) && r.ret == 0 && g_log.n == 0, m, &r);
        }
        for (int k = 0; k < 2; k++) {
            dt_world(0x9801 + k);
            W.nb->proto = 0; W.nb->mgr[0] = g_mgrs[0];
            *(int32_t*)(g_mgrs[0] + 0x24) = 3;
            W.nb->sel = 2;
            g_tables[0][0x54 * 2 + 0x2c] = (uint8_t)k;
            g_sn = 0; HS->script_n = HS->script_pos = 0;
            sc_key('p'); sc_key(0xd);
            fx_same(f, {U32(W.nb), 0}, k ? "connect, the last game listed (a password)" : "connect, the last game listed");
        }
    }
    // ---- MenuMultiChooseTransport: a long version translation; the LAN path with no game listed --------------------------------
    if (fx_on(9)) {
        const Ent& f = fx_fn("MenuMultiChooseTransport");
        FxPatch dl(F_UIDoDialog, (void*)&fx_dialog);
        auto setup = [&](uint32_t seed, int32_t ret, int32_t games) {
            dt_world(seed);
            g_fx_ret = ret;
            UI_GU32(S_GRP_LAN) = 1; UI_GU32(S_GRP_DIRECT) = 2; UI_GU32(S_GRP_MODEM) = 4;
            for (int k = 0; k < 2; k++) *(int32_t*)(g_mgrs[k] + 0x24) = games;
            memset(W.info, 0x5c, sizeof(MultiGenesisInfo));
            g_fx_version[0] = 0;
        };
        setup(0x9900, -1, 3);
        fx_xl(0x0057a5c4, 2, 0x0057a428, mk(0, 'V', 200));
        Result r = fx_run(f, true, {U32(W.info)});
        fx_check(fx_clean(f, r) && r.ret == 0 && fx_expect(63, "%s %d", g_fx_s[0], 0x25) == g_fx_version,
                 "MenuMultiChooseTransport, a 200-character version translation: clean, the text its first 63", &r);
        setup(0x9901, 0, 0);
        r = fx_run(f, true, {U32(W.info)});
        bool untouched = true;
        for (uint32_t i = 0; i < sizeof(MultiGenesisInfo); i++) untouched &= ((const uint8_t*)W.info)[i] == 0x5c;
        fx_check(fx_clean(f, r) && r.ret == 0 && untouched,
                 "MenuMultiChooseTransport, LAN chosen with no game listed: clean, 0 (as Back), the info untouched", &r);
        setup(0x9902, -1, 3);
        fx_xl(0x0057a5c4, 2, 0x0057a428, mk(0, 'V', 63 - 3));
        fx_same(f, {U32(W.info)}, "MenuMultiChooseTransport, a version text of 63 characters");
        setup(0x9903, 0, 1);
        fx_same(f, {U32(W.info)}, "MenuMultiChooseTransport, LAN, the one game listed");
    }
    mem_load(g_pristine);
    printf("fix tests: %s -- %d checks (%d of them original against rewrite), %d failed\n", g_fx_bad ? "FAILED" : "all passed", g_fx_n,
           g_fx_same_n, g_fx_bad);
    return g_fx_bad;
}
#endif

// ---- main -----------------------------------------------------------------------------------------------------------------------
static bool is_modal(const char* nm) {
    static const char* const m[] = {"MenuMultiChooseTransport", "MyDoInputBoxN", "MyDoCancelBox", "NetBrowser::new_server",
                                    "NetBrowser::FindHost", "NetBrowser::find_host", "NetBrowser::connect", "NetBrowser::NewServer",
                                    "NetBrowser::Connect", "LineControl::place_call", "LineControl::answer", "LineControl::line_check",
                                    "DirectLineControl::Connect", "ModemLineControl::Answer", "ModemLineControl::Call"};
    for (const char* s : m)
        if (!strcmp(nm, s)) return true;
    return strstr(nm, "::Added") != 0;
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
    char* s = strstr(exe, "\\test\\world_menu_multi.cpp");
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
        {F_OptionsGetF, (void*)&stub_OptionsGetF}, {F_OptionsGetI, (void*)&stub_OptionsGetI}, {F_OptionsGetB, (void*)&stub_OptionsGetB},
        {F_OptionsGetS, (void*)&stub_OptionsGetS}, {F_OptionsSetF, (void*)&stub_OptionsSetF}, {F_OptionsSetI, (void*)&stub_OptionsSetI},
        {F_OptionsSetB, (void*)&stub_OptionsSetB}, {F_OptionsSetS, (void*)&stub_OptionsSetS},
        {F_OptionsLoadModule, (void*)&stub_OptionsLoadModule}, {F_OptionsFlush, (void*)&stub_OptionsFlush},
        // this step's
        {0x004ae300, (void*)&stub_SocketCreateUDP}, {0x004ae3a0, (void*)&stub_SocketCreateIPX},
        {F_SocketCreateLine, (void*)&stub_SocketCreateLine}, {F_CreateSessionMgr, (void*)&stub_CreateSessionMgr},
        {F_SessionMgr_Tick, (void*)&stub_SM_Tick}, {F_SessionMgr_ServiceRequestBroadcast, (void*)&stub_SM_ServiceRequestBroadcast},
        {F_SessionMgr_GetServiceTable, (void*)&stub_SM_GetServiceTable},
        {F_SessionMgr_CanDestroySafely, (void*)&stub_SM_CanDestroySafely}, {F_SessionMgr_dtor, (void*)&stub_SM_dtor},
        {F_SessionMgr_AsyncServiceRequestByHostname, (void*)&stub_SM_AsyncServiceRequestByHostname},
        {F_SessionMgr_AsyncCancel, (void*)&stub_SM_AsyncCancel},
        {F_LineEnumerateDevices, (void*)&stub_LineEnumerateDevices}, {F_LineStatusText, (void*)&stub_LineStatusText},
        {F_LineDeviceInfo_GetError, (void*)&stub_LDI_GetError}, {F_LineChecker_ctor, (void*)&stub_LineChecker_ctor},
        {F_LineChecker_Tick, (void*)&stub_LineChecker_Tick}, {F_LineChecker_DoneChecking, (void*)&stub_LineChecker_DoneChecking},
        {F_LineChecker_LineOk, (void*)&stub_LineChecker_LineOk}, {F_LineChecker_IsServer, (void*)&stub_LineChecker_IsServer},
        {F_Win32Idle, (void*)&stub_Win32Idle}, {F_gxTextWidth, (void*)&stub_gxTextWidth},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    build_world();
    mem_save(g_pristine);
    printf("world: %d widgets in the running window, %u KB of heap used; %d ui / menu initialisers run at setup\n",
           (int)W.widgets.size(), (HS->heap_next - A_HEAP) / 1024, (int)g_setup_e.size());

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
            randomize_globals();
            randomize_stubs();
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
    const int dt_bad = !only || strstr("MenuMultiChooseTransport", only) ? directed_tests() : 0;
#if MENU_FIXES
    const int fix_bad = only ? 0 : directed_fix_tests();
#else
    const int fix_bad = 0;
#endif
    if (tags) tag_print();
    printf("%d functions (%d static initialisers; %d pure, %d replay_only, %d others), %d listed twice; %lld checks (%lld on poisoned .data), "
           "%lld logged words compared: %d differ, %d footprint violations, %d checks faulted (%lld in both, the same way), %d skipped; "
           "%d functions bad\n", g_nfns, n_e, pure_n, replay_n, g_nfns - pure_n - replay_n, dup, checks, poisoned_checks, log_words,
           differ, fp_bad, faults, both_fault, skipped, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    return differ || fp_bad || dup || dt_bad || fix_bad ? 1 : 0;
}

