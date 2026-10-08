// world_menu_sched.cpp -- multiplayer stage N3 (group B): every rewrite of hook/menu_sched.cpp (msched.obj, the lobby)
// against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_menu_sched.cpp
//        /Fo<dir>\ /Fe<dir>\world_menu_sched.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//     (and with /DVP_MENU_FIXES: the fix build, below)
//   run:   world_menu_sched.exe [rounds] [seed]      (VP_TRACE=1: one line per function; VP_ONLY=name: just those;
//          VP_TAGS=1: which stubs the originals reached; VP_DEBUG=1: the window's widgets at setup, every sprintf, and
//          where a fault happened)
//
// Built as test/world_menu_options.cpp is (its loader, raw call, memory comparison, toolkit stubs and logs are copied
// here): out\race_v10.exe loaded at 0x400000, hook/menu_sched.cpp included with PORT_FN redefined to list each function
// (its v1.0 address, the rewrite, its calling convention, stack arguments, return, footprint).
//
// The world. Once, at start: every $E static initialiser of the libraries `ui` and `menu` (the Xlators, the colours; from
// out\inventory.csv), the game's own UIBegin, a full-screen window made by WidgetCreateWindow and made the running one;
// a fake network -- a LiveMultiInfo, a race client (state, reason, the proposal and race info it hands out, up to 8 users
// with names and statuses, a chat scrollback ring of ChatLines with whispers and runs of one speaker), a race server, a
// session -- whose objects have logging vtables; and in the window, by the game's own _UIAddItems, the lobby's controls
// built by their ORIGINAL constructors exactly as MultiDo builds them (a LiveMultiKiller, the ChatControl, the
// MultiTrackChooser for the host, the MultiMaster -- which MemAllocs the host's MultiRaceCfg -- the MultiRaceInfo, the
// MultiCarChooser) and filled by their original Addeds. This is the pristine state.
//
// Then for EVERY listed function, `rounds` times (default 24; ten times that for one that runs no dialog): pristine
// restored; half the rounds POISONED (every byte of .data/.bss random, except what's read as a string or followed as a
// pointer, as world_menu_options.cpp does); the network random (the client's state 0..13 -- the lobby's: see the FIX
// CANDIDATE in MultiMaster::Update --, its reason, the proposal present or not, its slaved-cars flag and track, the race
// info, the users and the chat ring, the server there or not -- the host or a client --, its answers, MultiEnabled,
// AmOwner, the creations failing or not), the controls' fields random (the picked user, the chat line -- empty, short,
// over 47 characters with and without spaces --, the master's state, flags and times, the car chooser's arrows and Setup,
// the track and the host flag, the race configuration's settings and sliders), the function-local Xlators constructed or
// not; arguments made for the function (the right object for a method, a fresh random block for a constructor, a canvas,
// a callback's id, a NetProposal or NetRaceInfo, a MultiGenesisInfo for MenuMultiScheduler, a chat line for
// break_chatline); for a function that runs a dialog (MultiDo, MenuMultiScheduler, the yes / no and lost-connection
// boxes) a random input script -- mouse moves onto the running window's widgets, presses and releases, keys, idle frames
// -- ended by a watchdog after 40 frames if the dialog doesn't end itself. The ORIGINAL runs from a raw call thunk, its
// results kept, the snapshot restored, the REWRITE runs the same way. Compared: every byte of .data/.bss/.idata and the
// 1 MB arena (a dword where both passes left a pointer into this thread's stack counts as equal), the return value, the
// call logs of every stub (the fake network's methods included), faults, the registers. Every byte the original changed
// must lie in the rewrite's footprint (unless it's replay_only); a pure one changes nothing. Built VP_FAITHFUL: every
// rewrite must match its original bit for bit.
//
// Inputs are kept inside the original's bounds where it would overrun a buffer on its own stack (the cases the FIXes in
// hook/menu_sched.cpp are for): track names of at most 11 characters, car names of at most 20, NumberString's argument in
// 0..14, the client's state in 0..13, set_arrows / set_garage's flag 0 or 1. So every round is a case the fixes leave alone.
//
// Built with /DVP_MENU_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Multiplayer
// menus"): every function is still compared as above (the rounds never reach a fixed case), and then directed_fix_tests
// runs each fix on its bad case -- the rewrite alone, clean, nothing written outside what it's given, the promised result
// -- and its boundary case on both, bit for bit (see there). VP_FIXTEST=n runs fix group n alone; built with
// /DVP_MENU_FIXES /DVP_FAITHFUL the same tests run on the faithful rewrites, and every group fails (or crashes).
//
// Stubbed (a jump to a logger; state in the arena so both passes see the same): world_ui.cpp's (MemAlloc / delete, the
// logs, the input and the clock, the 2D calls, the fonts and stamps, the palettes, Xlator::xlate, sprintf, the files,
// atexit, the CRT's fatal paths) and this step's callees outside msched.obj: the options (OptionsGet / OptionsSet, int
// and byte), the front end's tables (the tracks, their friendly names, the cars, the realism / race type / weather / time
// / AI strength strings, the laps), HackGetCarIndex, ResourceSetMustLoad / Unload, MenuDoOptions, MenuEditCar, group C's
// CarViewer3D (its constructor writes the car's index where MultiCarChooser reads it), and the multi library
// (LiveMultiInfo::Grab / Release / AmOwner, MultiEnabled, MultiResumeChat, MultiLiveEnd, SessionMgr::Tick /
// SetConnectPassword, CreateSessionMgr, CreateRaceClient x3, CreateRaceServer, GetClientStatusString,
// ClientDisconnectString). The game's own code runs everywhere else: the whole widget toolkit (UIDoDialog, _UIAddItems,
// the widgets, UIDoYesNoBox, UIDoDratBox, CreateMultiString, UICustomControl), UIDialogItem's and Xlator's constructors,
// HackEnabled, __ftol, and every other msched.obj function (the originals, which the rewrites call by address).
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
#include <string>
#include <utility>
#include <vector>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif

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

#include "../hook/menu_sched.cpp"

// N0's patched sites, as the stock calls (no session here): the game's own functions (stubbed below)
void __fastcall net_Grab(void* lmi, void*) { ((void(__fastcall*)(void*, int))(uintptr_t)0x004a2d10)(lmi, 0); }
void __fastcall net_Release(void* lmi, void*) { ((void(__fastcall*)(void*, int))(uintptr_t)0x004a2e50)(lmi, 0); }

using namespace uit;
using namespace msc;

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
    int32_t nusers, user_ids[8], me, maxname;
    uint8_t changes, holding, all_approved, multi_enabled, am_owner, hack_on, proposal_on, scroll_on;
    int32_t car, hack_car, car_number, paintjob, ntracks, laps;
    uint32_t service_id;
    uint8_t sm_ok, server_ok, client_ok, disc_named;
};
#define HS ((HState*)g_arena)
enum : uint32_t { A_HEAP = 0x60000 };                     // the bump heap: arena + 0x60000 .. the end

// ---- the fake network: a race client, a race server, a session (in the arena), their vtables (here) ---------------------------
#pragma pack(push, 1)
struct FChatLine {                                         // net_race.h's ChatLine (0x4c)
    int32_t user;
    char text[0x32];
    char name[13];
    uint8_t whisper;
    FChatLine* prev;
    FChatLine* next;
};
#pragma pack(pop)
static_assert(sizeof(FChatLine) == 0x4c, "ChatLine");
struct FakeClient {
    const void* vtbl;
    int32_t state;                                         // +4
    int32_t reason;                                        // +8
    uint8_t proposal[0x3c];
    uint8_t race[0x34];
    FChatLine ring[8];
    int32_t nlines;
};
struct FakeServer { const void* vtbl; int32_t k; };
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
// the fix tests' overrides (directed_fix_tests; none set in the rounds)
static bool g_fx_th_set;
static int32_t g_fx_th;
static const char* const* g_fx_tn;                         // the track names (GetTrackName), 64 of them
static const char *g_fx_tfn, *g_fx_car, *g_fx_carf, *g_fx_str;   // a friendly track name, a car's name and friendly name, the front end's strings
static char g_fx_lap[0x400], g_fx_rsml[0x400];             // GetLapCountFromType's name, ResourceSetMustLoad's last
static int32_t __cdecl stub_gxTextHeight() { L('TXTH'); return g_fx_th_set ? g_fx_th : 8; }
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
// an Xlator's text: its key from after its last ':' ("Multi:AI_Cars" -> "AI_Cars"), as short as a real translation (the
// whole keys overrun the ghost types' 64-byte multi string at 0x57a048: a FIX in hook/menu_sched.cpp)
static uint32_t xl_text(uint32_t key) {
    const char* k = (const char*)(uintptr_t)key;
    if (!readable(k, 1)) return key;
    const char* c = strrchr(k, ':');
    return c ? (uint32_t)(uintptr_t)(c + 1) : key;
}
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
    if (getenv("VP_DEBUG")) printf("    sprintf %s (%08x %08x %08x)\n", readable(fmt, 1) ? fmt : "?", w[0], w[1], w[2]);
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
static FakeClient* g_client;                               // the world's (in the arena)
static FakeServer* g_server;
static void* g_sm;
// the options: a key is present (its value written) or missing (the caller's value logged), by a hash of the section,
// the key and the round
static uint32_t opt_h(const char* sec, const char* key) {
    return (hash_str(readable(sec, 1) ? sec : "") * 31u) ^ hash_str(readable(key, 1) ? key : "") ^ HS->opt_seed;
}
static bool opt_present(uint32_t h) { return (h & 7) != 0; }
static void L_key(const char* sec, const char* key) { LS(sec, 32); LS(key, 64); }
static void __cdecl stub_OptionsGetI(const char* sec, const char* key, int32_t* v) {
    L('OGI '); L_key(sec, key); L(P(v));
    const uint32_t h = opt_h(sec, key);
    if (!opt_present(h)) { L('MISS'); L((uint32_t)*v); return; }
    *v = (int32_t)((h >> 3) % 6);                       // 0..5: NumberString's range (outside it: a FIX)
}
static void __cdecl stub_OptionsGetB(const char* sec, const char* key, uint8_t* v) {
    L('OGB '); L_key(sec, key); L(P(v));
    const uint32_t h = opt_h(sec, key);
    if (!opt_present(h)) { L('MISS'); L(*v); return; }
    *v = (uint8_t)(((h >> 3) & 15) == 0 ? (h >> 7) : (h >> 3) & 1);
}
static void __cdecl stub_OptionsSetI(const char* sec, const char* key, int32_t v) { L('OSI '); L_key(sec, key); L((uint32_t)v); }
static void __cdecl stub_OptionsSetB(const char* sec, const char* key, uint32_t v) { L('OSB '); L_key(sec, key); L(v & 0xff); }
// the front end's tables
static const char* k_cars[8] = {"viper", "gts", "rt10", "acr", "jeep", "willys", "cop", "ghost"};
static const char* k_car_names[8] = {"Viper RT/10", "Viper GTS", "RT/10 '92", "ACR", "Indy Jeep", "Willys MB", "Police", "Ghost car 20 chars."};
static const char* k_tracks[8] = {"bemidji", "nfield", "kenyon", "coliseum", "rotunda", "temple11chr", "random", "hell"};
static const char* k_track_names[8] = {"Bemidji", "Northfield", "Kenyon", "The Coliseum", "Temple Rotunda", "Temple", "Random", "Hell"};
static const char* k_strs[6][4] = {
    {"Rookie", "Pro", "Expert", "?realism"}, {"Single", "Laps", "Drag", "?type"}, {"Clear", "Rain", "Fog", "?weather"},
    {"Noon", "Dusk", "Night", "?time"}, {"Easy", "Medium", "Hard", "?ai"}, {"Status A", "Status B", "Lost", "?status"},
};
static const char* k_user_names[8] = {"Player", "Kate", "a12charname!", "x", "Racer Jane", "", "Road-Hawk", "Night Wolf"};
static int32_t __cdecl stub_HackGetCarIndex() { L('HGCI'); return HS->hack_car; }
static int32_t __cdecl stub_GetCarFileNumber(const char* s) { L('GCFR'); LS(s); return HS->car_number; }
static int32_t __cdecl stub_GetTrackCount() { L('GTCN'); return HS->ntracks; }
static const char* __cdecl stub_GetTrackName(int32_t i) { L('GTNM'); L((uint32_t)i); return g_fx_tn ? g_fx_tn[(uint32_t)i & 63] : k_tracks[(uint32_t)i & 7]; }
static const char* __cdecl stub_GetTrackFriendlyName(int32_t i) { L('GTFN'); L((uint32_t)i); return g_fx_tfn ? g_fx_tfn : k_track_names[(uint32_t)i & 7]; }
static const char* __cdecl stub_GetRealismString(int32_t i) { L('GRLS'); L((uint32_t)i); return g_fx_str ? g_fx_str : k_strs[0][(uint32_t)i & 3]; }
static const char* __cdecl stub_GetRaceTypeString(int32_t i) { L('GRTS'); L((uint32_t)i); return g_fx_str ? g_fx_str : k_strs[1][(uint32_t)i & 3]; }
static const char* __cdecl stub_GetWeatherString(int32_t i) { L('GWES'); L((uint32_t)i); return g_fx_str ? g_fx_str : k_strs[2][(uint32_t)i & 3]; }
static const char* __cdecl stub_GetGameTimeString(int32_t i) { L('GGTS'); L((uint32_t)i); return g_fx_str ? g_fx_str : k_strs[3][(uint32_t)i & 3]; }
static const char* __cdecl stub_GetAIStrengthString(int32_t i) { L('GAIS'); L((uint32_t)i); return g_fx_str ? g_fx_str : k_strs[4][(uint32_t)i & 3]; }
static int32_t __cdecl stub_GetLapCountFromType(int32_t t, const char* name) {
    L('GLAP'); L((uint32_t)t); LS(name);
    if (readable(name, 1)) strncpy(g_fx_lap, name, sizeof g_fx_lap - 1);
    return HS->laps + t;
}
static void __cdecl stub_ResourceSetMustLoad(const char* s) { L('RSML'); LS(s); if (readable(s, 1)) strncpy(g_fx_rsml, s, sizeof g_fx_rsml - 1); }
static void __cdecl stub_ResourceSetUnload(const char* s) { L('RSUL'); LS(s); }
static void __cdecl stub_MenuDoOptions() { L('MOPT'); HS->hack_car = (HS->hack_car + 3) & 7; }
static uint8_t __cdecl stub_MenuEditCar(const char* car, const char* track, uint32_t a, uint32_t b) {
    L('MEDC'); LS(car); LS(track); L(a); L(b);
    return 1;
}
// group C's CarViewer3D: a UICustomControl (a logging vtable: MultiCarChooser::Added adds it to the window), the car's
// index at +0x18 (MultiCarChooser reads it), the rest a pattern
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
static void* __fastcall stub_CV_ctor(uint8_t* self, int, int32_t car, uint32_t fmax, uint32_t fname) {
    L('CVCT'); L(P(self)); L((uint32_t)car); L(fmax); L(fname);
    for (int i = 0; i < 0x110; i++) self[i] = (uint8_t)(i * 13 + car);
    *(void**)self = g_cc_vtbl;
    *(uint32_t*)(self + 0x14) = 0;
    *(int32_t*)(self + CV_CAR) = car;
    return self;
}
static void __fastcall stub_CV_dtor(uint8_t* self, int) { L('CVDT'); L(P(self)); }
static void __fastcall stub_CV_Next(uint8_t* self, int) { L('CVNX'); L(P(self)); *(int32_t*)(self + CV_CAR) = (*(int32_t*)(self + CV_CAR) + 1) & 7; }
static void __fastcall stub_CV_Prev(uint8_t* self, int) { L('CVPV'); L(P(self)); *(int32_t*)(self + CV_CAR) = (*(int32_t*)(self + CV_CAR) + 7) & 7; }
static void __fastcall stub_CV_SetCar(uint8_t* self, int, int32_t car) { L('CVSC'); L(P(self)); L((uint32_t)car); *(int32_t*)(self + CV_CAR) = car; }
static int32_t __cdecl cv_car(const uint8_t* self) { return readable(self + CV_CAR, 4) ? *(const int32_t*)(self + CV_CAR) : 0; }
static int32_t __fastcall stub_CV_GetPaintJob(uint8_t* self, int) { L('CVPJ'); L(P(self)); return HS->paintjob; }
static const char* __fastcall stub_CV_GetFriendlyName(uint8_t* self, int) { L('CVFN'); L(P(self)); return g_fx_carf ? g_fx_carf : k_car_names[(uint32_t)cv_car(self) & 7]; }
static const char* __fastcall stub_CV_GetName(uint8_t* self, int) { L('CVGN'); L(P(self)); return g_fx_car ? g_fx_car : k_cars[(uint32_t)cv_car(self) & 7]; }
// the multi library
static uint8_t __cdecl stub_MultiEnabled() { L('MENB'); return HS->multi_enabled; }
static void __cdecl stub_MultiResumeChat() { L('MRCH'); }
static void __cdecl stub_MultiLiveEnd() { L('MLEN'); }
static void __fastcall stub_LMI_Grab(void* self, int) { L('GRAB'); L(P(self)); }
static void __fastcall stub_LMI_Release(void* self, int) { L('RELE'); L(P(self)); }
static uint8_t __fastcall stub_LMI_AmOwner(void* self, int) { L('OWNR'); L(P(self)); return HS->am_owner; }
static void __fastcall stub_SM_Tick(void* self, int) { L('SMTK'); L(P(self)); }
static void __fastcall stub_SM_SetConnectPassword(void* self, int, const char* pw) { L('SMPW'); L(P(self)); LS(pw, 32); }
static void* __cdecl stub_CreateSessionMgr(void* sock, int32_t a, int32_t b, int32_t c) {
    L('CSMG'); L(P(sock)); L((uint32_t)a); L((uint32_t)b); L((uint32_t)c);
    return HS->sm_ok ? g_sm : 0;
}
static void* __cdecl stub_CreateRaceClient_rs(void* sm, const void* rs) { L('CRC1'); L(P(sm)); L(P(rs)); return HS->client_ok ? (void*)g_client : 0; }
static void* __cdecl stub_CreateRaceClient_name(void* sm, const char* nm) { L('CRC2'); L(P(sm)); LS(nm, 64); return HS->client_ok ? (void*)g_client : 0; }
static void* __cdecl stub_CreateRaceClient_id(void* sm, uint32_t id) { L('CRC3'); L(P(sm)); L(id); return HS->client_ok ? (void*)g_client : 0; }
static void* __cdecl stub_CreateRaceServer(void* sm, const char* nm, const char* pw) {
    L('CRSV'); L(P(sm)); LS(nm, 64); L(P(pw)); if (pw) LS(pw, 32);
    return HS->server_ok ? (void*)g_server : 0;
}
static const char* __cdecl stub_GetClientStatusString(int32_t s) { L('GCSS'); L((uint32_t)s); return g_fx_str ? g_fx_str : k_strs[5][(uint32_t)s & 3]; }
static const char* __cdecl stub_ClientDisconnectString(int32_t r) { L('CDSS'); L((uint32_t)r); return HS->disc_named ? "Kicked" : 0; }
// the race client's methods (thiscall; each pops its own arguments)
static void __fastcall fc_void(FakeClient* c, int) { L('FCV '); L(P(c)); }
static void __fastcall fc_tick(FakeClient* c, int) { L('FCTK'); L(P(c)); }
static uint8_t __fastcall fc_changes(FakeClient* c, int) { L('FCCH'); L(P(c)); return HS->changes; }
static int32_t __fastcall fc_status(FakeClient* c, int, int32_t id) { L('FCST'); L(P(c)); L((uint32_t)id); return (id * 7) & 3; }
static int32_t __fastcall fc_count(FakeClient* c, int) { L('FCCU'); L(P(c)); return HS->nusers; }
static int32_t __fastcall fc_first(FakeClient* c, int, int32_t* it) {
    L('FCFU'); L(P(c)); L(P(it));
    *it = 0;
    return HS->nusers > 0 ? HS->user_ids[0] : 0;
}
static int32_t __fastcall fc_next(FakeClient* c, int, int32_t* it) {
    L('FCNU'); L(P(c)); L(P(it)); L((uint32_t)*it);
    const int32_t i = ++*it;
    return i >= 0 && i < HS->nusers ? HS->user_ids[i] : 0;
}
static int32_t __fastcall fc_nth(FakeClient* c, int, int32_t i) {
    L('FCNT'); L(P(c)); L((uint32_t)i);
    return i >= 0 && i < HS->nusers ? HS->user_ids[i] : 0;
}
static int32_t __fastcall fc_me(FakeClient* c, int) { L('FCME'); L(P(c)); return HS->me; }
static int32_t __fastcall fc_maxname(FakeClient* c, int) { L('FCMN'); L(P(c)); return HS->maxname; }
static const char* __fastcall fc_username(FakeClient* c, int, int32_t id) { L('FCUN'); L(P(c)); L((uint32_t)id); return k_user_names[(uint32_t)id & 7]; }
static void __fastcall fc_speak(FakeClient* c, int, const char* s) { L('FCSP'); L(P(c)); L(P(s)); LS(s, 0x90); }
static void __fastcall fc_whisper(FakeClient* c, int, const char* s, int32_t id) { L('FCWH'); L(P(c)); L(P(s)); LS(s, 0x90); L((uint32_t)id); }
static const void* __fastcall fc_scrollback(FakeClient* c, int) { L('FCSB'); L(P(c)); return HS->scroll_on && c->nlines ? (const void*)&c->ring[c->nlines - 1] : 0; }
static void __fastcall fc_proceed_car(FakeClient* c, int) { L('FCPC'); L(P(c)); }
static void __fastcall fc_proceed_race(FakeClient* c, int, const char* name, int32_t a) { L('FCPR'); L(P(c)); LS(name); L((uint32_t)a); }
static void __fastcall fc_back(FakeClient* c, int) { L('FCBK'); L(P(c)); }
static uint8_t __fastcall fc_holding(FakeClient* c, int) { L('FCHO'); L(P(c)); return HS->holding; }
static int32_t __fastcall fc_getcar(FakeClient* c, int) { L('FCGC'); L(P(c)); return HS->car; }
static const void* __fastcall fc_raceinfo(FakeClient* c, int) { L('FCRI'); L(P(c)); return c->race; }
static const void* __fastcall fc_proposal(FakeClient* c, int) { L('FCPP'); L(P(c)); return HS->proposal_on ? (const void*)c->proposal : 0; }
static void* g_client_vtbl[0x24] = {
    (void*)&fc_void, (void*)&fc_tick, (void*)&fc_void, (void*)&fc_void, (void*)&fc_changes, (void*)&fc_status, (void*)&fc_count,
    (void*)&fc_first, (void*)&fc_next, (void*)&fc_nth, (void*)&fc_me, (void*)&fc_maxname, (void*)&fc_username, (void*)&fc_void,
    (void*)&fc_speak, (void*)&fc_whisper, (void*)&fc_scrollback, (void*)&fc_proceed_car, (void*)&fc_proceed_race, (void*)&fc_back,
    (void*)&fc_void, (void*)&fc_void, (void*)&fc_void, (void*)&fc_holding, (void*)&fc_getcar, (void*)&fc_raceinfo, (void*)&fc_proposal,
    (void*)&fc_void, (void*)&fc_void, (void*)&fc_void, (void*)&fc_void, (void*)&fc_void, (void*)&fc_void, (void*)&fc_void,
    (void*)&fc_void, (void*)&fc_void,
};
// the race server's
static void __fastcall fs_void(FakeServer* s, int) { L('FSV '); L(P(s)); }
static void __fastcall fs_tick(FakeServer* s, int) { L('FSTK'); L(P(s)); }
static void __fastcall fs_propose(FakeServer* s, int, const uint8_t* p, int32_t me) { L('FSPP'); L(P(s)); L(P(p)); LN((const char*)p, 0x3c); L((uint32_t)me); }
static uint8_t __fastcall fs_all_approved(FakeServer* s, int) { L('FSAA'); L(P(s)); return HS->all_approved; }
static void __fastcall fs_approve(FakeServer* s, int) { L('FSAP'); L(P(s)); }
static void __fastcall fs_unapprove(FakeServer* s, int) { L('FSUA'); L(P(s)); }
static void __fastcall fs_setcar(FakeServer* s, int, int32_t idx, const char* name) { L('FSSC'); L(P(s)); L((uint32_t)idx); LS(name); }
static uint32_t __fastcall fs_service(FakeServer* s, int) { L('FSID'); L(P(s)); return HS->service_id; }
static void* g_server_vtbl[0x14] = {
    (void*)&fs_void, (void*)&fs_void, (void*)&fs_tick, (void*)&fs_void, (void*)&fs_propose, (void*)&fs_all_approved,
    (void*)&fs_approve, (void*)&fs_unapprove, (void*)&fs_setcar, (void*)&fs_void, (void*)&fs_void, (void*)&fs_void,
    (void*)&fs_service, (void*)&fs_void, (void*)&fs_void, (void*)&fs_void, (void*)&fs_void, (void*)&fs_void, (void*)&fs_void,
    (void*)&fs_void,
};
// ---- the raw call (world_ui.cpp's) --------------------------------------------------------------------------------------------
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
    if (getenv("VP_DEBUG")) {
        printf("    fault at %08x: edi %08x esi %08x ecx %08x; code addresses on the stack:", (uint32_t)e->ContextRecord->Eip,
               (uint32_t)e->ContextRecord->Edi, (uint32_t)e->ContextRecord->Esi, (uint32_t)e->ContextRecord->Ecx);
        const uint32_t* sp = (const uint32_t*)(uintptr_t)e->ContextRecord->Esp;
        int k = 0;
        for (int i = 0; i < 0x400 && k < 12; i++)
            if (sp[i] >= 0x401000 && sp[i] < 0x4db000) { printf(" %08x", sp[i]); k++; }
        printf("\n");
    }
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}
// the stack the call will use, filled with one pattern (so a local the original leaves uninitialised reads the same thing
// in both passes). Called last before the raw call, and written in assembly, top down (the guard pages grow in order): a
// C function's own frame and the calls made after it (_controlfp_s) would leave their return addresses and locals in the
// region the callee's frame uses, at depths the two passes' frames don't share.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((naked)) static void fill_stack() {
    __asm__ volatile(
        "push edi\n\t"
        "push ecx\n\t"
        "push eax\n\t"
        "lea edi, [esp - 4]\n\t"
        "mov ecx, 0xc000\n\t"
        "mov eax, 0xcdcdcdcd\n\t"
        "std\n\t"
        "rep stosd\n\t"
        "cld\n\t"
        "pop eax\n\t"
        "pop ecx\n\t"
        "pop edi\n\t"
        "ret");
}
#else
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
#endif
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

static void fill_random(void* p, uint32_t n) { uint8_t* b = (uint8_t*)p; for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)rnd(); }

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

struct World {
    gxCanvas* screen;
    WidgetWindow* win;
    uint8_t* lmi;                                   // LiveMultiInfo (0x3c)
    LiveMultiKiller* killer;
    ChatControl* chat;
    MultiTrackChooser* track;
    MultiRaceInfo* info;
    MultiCarChooser* carch;
    MultiMaster* master;
    MultiRaceCfg* cfg;
    int32_t* ints;                                  // int* arguments
    char* scratch;                                  // fresh blocks for constructors (0x800)
    char* line;                                     // break_chatline's argument (0x200)
    uint8_t* prop;                                  // a NetProposal argument (0x40)
    uint8_t* gi;                                    // a MultiGenesisInfo (0x80)
    uint8_t* lmi2;                                  // a LiveMultiInfo for MenuMultiScheduler (0x40)
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
static void set_chat_ring(FakeClient* c, int n) {
    c->nlines = n;
    for (int i = 0; i < 8; i++) {
        FChatLine* l = &c->ring[i];
        memset(l, 0, sizeof *l);
        l->user = HS->user_ids[irange(0, 7)];
        if (chance(30) && i) l->user = c->ring[i - 1].user;          // a run of one speaker
        snprintf(l->text, sizeof l->text, "line %d of the chat %u", i, rnd() % 1000);
        if (chance(25)) {                                            // a full line: 49 characters, as Speak cuts it
            for (int k = 0; k < 0x31; k++) l->text[k] = (char)('a' + (i * 7 + k) % 26);
            l->text[0x31] = 0;
        }
        snprintf(l->name, sizeof l->name, "%s", k_user_names[irange(0, 7)]);
        l->whisper = (uint8_t)(chance(25) ? 1 : 0);
    }
    for (int i = 0; i < n; i++) {
        c->ring[i].prev = &c->ring[(i + n - 1) % n];
        c->ring[i].next = &c->ring[(i + 1) % n];
    }
}
static void build_world() {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    HS->ntracks = 8; HS->opt_seed = 0x1234567; HS->nusers = 3; HS->user_ids[0] = 0x101; HS->user_ids[1] = 0x202; HS->user_ids[2] = 0x303;
    HS->proposal_on = 1; HS->scroll_on = 1; HS->sm_ok = HS->server_ok = HS->client_ok = 1; HS->car_number = 0; HS->maxname = 8;
    UI_G32(S_SCREEN_W) = 640;
    UI_G32(S_SCREEN_H) = 480;
    for (uint32_t a : g_setup_e) call_orig(a, 0, {});
    call_orig(0x004779a0, 0, {});                   // UIBegin
    W.screen = (gxCanvas*)wv(sizeof(gxCanvas));
    W.screen->format = 4; W.screen->pixels = (uint8_t*)0x7b000000; W.screen->w = 640; W.screen->h = 480; W.screen->pitch = 1280;
    W.screen->cx1 = 640; W.screen->cy1 = 480;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    W.ints = (int32_t*)wv(64 * 4);
    W.scratch = (char*)wv(0x800);
    W.line = (char*)wv(0x200);
    W.prop = (uint8_t*)wv(0x40);
    W.gi = (uint8_t*)wv(0x80);
    W.lmi2 = (uint8_t*)wv(0x40);
    // the fake network
    g_client = (FakeClient*)wv(sizeof(FakeClient));
    g_client->vtbl = g_client_vtbl;
    g_client->state = 9;
    set_chat_ring(g_client, 4);
    g_server = (FakeServer*)wv(sizeof(FakeServer));
    g_server->vtbl = g_server_vtbl;
    g_sm = wv(0x40);
    W.lmi = (uint8_t*)wv(0x40);
    W.lmi[LMI_LIVE] = 1;
    *(void**)(W.lmi + LMI_SERVER) = g_server;
    *(void**)(W.lmi + LMI_CLIENT) = g_client;
    *(void**)(W.lmi + LMI_SM) = g_sm;
    UI_GP(void, S_POSTRACE_LMI) = W.lmi;
    // the lobby's controls, by their original constructors, as MultiDo builds them
    W.killer = (LiveMultiKiller*)wv(sizeof(LiveMultiKiller));
    W.killer->vtbl = (const void*)(uintptr_t)VT_LiveMultiKiller;
    W.killer->widget = 0;
    W.killer->lmi = W.lmi;
    W.chat = (ChatControl*)wv(sizeof(ChatControl));
    W.track = (MultiTrackChooser*)wv(sizeof(MultiTrackChooser));
    W.info = (MultiRaceInfo*)wv(sizeof(MultiRaceInfo));
    W.carch = (MultiCarChooser*)wv(sizeof(MultiCarChooser));
    W.master = (MultiMaster*)wv(sizeof(MultiMaster));
    call_orig(F_ChatControl_ctor, 1, {U32(W.chat), 0, U32(g_client)});
    call_orig(F_MultiTrackChooser_ctor, 1, {U32(W.track), 0, 1});
    call_orig(F_MultiRaceInfo_ctor, 1, {U32(W.info), 0});
    call_orig(F_MultiCarChooser_ctor, 1, {U32(W.carch), 0});
    call_orig(F_MultiMaster_ctor, 1, {U32(W.master), 0, U32(W.lmi)});
    W.cfg = (MultiRaceCfg*)W.master->cfg;
    // a full-screen window, running, and the controls added to it by the game's _UIAddItems
    call_orig(F_WidgetCreateWindow, 0, {U32("multi.stp"), 0, 0, 640, 480, 2});
    W.win = (WidgetWindow*)(uintptr_t)g_r_eax;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    W.items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 12);
    UICustomControl* ctl[6] = {W.killer, W.chat, W.track, W.master, W.info, W.carch};
    const int32_t pos[6][4] = {{0, 0, 0, 0}, {0x1fa, 0x136, 0x64, 0x58}, {0x19, 0x52, 0xb3, 0x91}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    for (int k = 0; k < 6; k++) {
        UIDialogItem* it = &W.items[k];
        it->type = 0x17; it->x = pos[k][0]; it->y = pos[k][1]; it->w = pos[k][2]; it->h = pos[k][3]; it->text = "";
        it->data = ctl[k];
    }
    call_orig(F_UIAddItems, 0, {U32(W.win), U32(W.items)});
    for (int i = 0; i < W.win->count; i++) W.widgets.push_back((Widget*)W.win->widgets[i]);
    if (getenv("VP_DEBUG"))
        for (int i = 0; i < W.win->count; i++) {
            const Widget* g = W.win->widgets[i];
            printf("  widget %d vt %08x", i, U32(g->vtbl));
            if (U32(g->vtbl) == VT_CustomWidget) { const CustomWidget* c = (const CustomWidget*)g; printf(" ctrl %08x vt %08x", U32(c->ctrl), c->ctrl ? U32(c->ctrl->vtbl) : 0); }
            printf("\n");
        }
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
static uint32_t fbits_(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void random_race(uint8_t* ri) {
    fill_random(ri, 0x34);
    *(int32_t*)(ri + NRI_REALISM) = irange(-1, 3);
    *(int32_t*)(ri + NRI_TIME) = irange(0, 3);
    *(int32_t*)(ri + NRI_WEATHER) = irange(0, 3);
    *(int32_t*)(ri + NRI_TYPE) = irange(0, 3);
    *(int32_t*)(ri + NRI_LAPS) = chance(30) ? 1 : irange(0, 12);
    *(int32_t*)(ri + NRI_STRENGTH) = irange(0, 3);
    ri[NRI_DAMAGE] = (uint8_t)(chance(50) ? 0 : rnd());
    ri[NRI_REVERSED] = (uint8_t)(chance(50) ? 0 : rnd());
    *(int32_t*)(ri + NRI_TRACK) = irange(0, 5);                          // a track the chooser has (any other: set_track's FIX)
    *(int32_t*)(ri + NRI_COUNT) = irange(0, 14);                         // NumberString's range (outside it: a FIX)
}
static void random_proposal(uint8_t* p) {
    fill_random(p, 0x3c);
    p[NP_IROC] = (uint8_t)(chance(50) ? 0 : chance(80) ? 1 : rnd());
    random_race(p + NP_RACE);
    *(int32_t*)(p + NP_TRACK) = irange(0, 5);                            // a track the chooser has (set_track's names)
}
static void random_line(char* s, int maxn) {
    const int n = chance(15) ? 0 : chance(40) ? irange(1, 47) : irange(48, maxn);
    for (int i = 0; i < n; i++) s[i] = chance(12) ? (char)" \t\n"[rnd() % 3] : (char)('a' + rnd() % 26);
    s[n] = 0;
}
static void randomize_world() {
    for (Widget* w : W.widgets) {
        w->over = chance(40); w->focus = chance(30); w->visible = chance(85); w->enabled = chance(85); w->dirty = chance(50);
    }
    WidgetWindow* w = W.win;
    w->over = chance(50) && w->count ? w->widgets[rnd() % (uint32_t)w->count] : 0;
    w->captured = 0;
    w->focus = chance(60) && w->count ? w->widgets[rnd() % (uint32_t)w->count] : 0;
    w->hidden = chance(30) ? rnd() & 0xffff : 0;
    w->disabled = chance(30) ? rnd() & 0xffff : 0;
    w->full_redraw = chance(40);
    UI_GP(WidgetWindow, S_ACTIVE) = chance(95) ? W.win : 0;
    UI_G8(S_EXIT) = chance(10);
    UI_GF(S_UI_DT) = range(0.0f, 0.25f);
    UI_G32(S_MOUSE_X) = irange(-10, 650); UI_G32(S_MOUSE_Y) = irange(-10, 490);
    // the network
    HS->nusers = irange(0, 8);
    for (int i = 0; i < 8; i++) HS->user_ids[i] = chance(5) ? 0 : (int32_t)((rnd() & 0xff) << 8 | (uint32_t)i);
    HS->me = chance(20) ? 0 : HS->user_ids[0];
    // directed (ChatWindow::Draw's line): now and then a padding far past any name, so that with a full chat line the
    // sprintf runs up to the end of the original's frame (252 bytes from the line: 0xc7 + ": " + 49 + NUL = 251)
    HS->maxname = chance(15) ? irange(0xb0, 0xc7) : irange(0, 12);
    HS->changes = (uint8_t)(rnd() & 1); HS->holding = (uint8_t)(chance(30) ? 1 : 0); HS->all_approved = (uint8_t)(rnd() & 1);
    HS->multi_enabled = (uint8_t)(rnd() & 1); HS->am_owner = (uint8_t)(rnd() & 1); HS->proposal_on = (uint8_t)(chance(80) ? 1 : 0);
    HS->scroll_on = (uint8_t)(chance(85) ? 1 : 0);
    HS->car = chance(20) ? -1 : irange(0, 7); HS->hack_car = irange(0, 7); HS->car_number = irange(0, 7); HS->paintjob = irange(-2, 5);
    HS->laps = irange(0, 9);
    HS->service_id = rnd();
    HS->sm_ok = (uint8_t)(chance(90) ? 1 : 0); HS->server_ok = (uint8_t)(chance(90) ? 1 : 0); HS->client_ok = (uint8_t)(chance(90) ? 1 : 0);
    HS->disc_named = (uint8_t)(rnd() & 1);
    FakeClient* c = g_client;
    c->state = chance(70) ? irange(9, 13) : irange(0, 13);
    c->reason = irange(-1, 5);
    random_proposal(c->proposal);
    random_race(c->race);
    set_chat_ring(c, irange(1, 8));
    *(void**)(W.lmi + LMI_SERVER) = chance(60) ? (void*)g_server : 0;
    W.lmi[LMI_LIVE] = (uint8_t)(rnd() & 1);
    UI_G8(0x004e52e4) = (uint8_t)(chance(50) ? 1 : 0);                   // HackEnabled
    // the controls
    ChatControl* ch = W.chat;
    ch->dragging = (uint8_t)(rnd() & 1);
    ch->sel = chance(30) ? -1 : irange(0, 9);
    random_line(ch->text, 0x7f);
    ch->users = irange(0, 8);
    ch->sel_id = chance(50) && HS->nusers ? HS->user_ids[irange(0, HS->nusers - 1)] : (int32_t)rnd();
    ch->window.last = chance(50) ? (const void*)&c->ring[irange(0, 7)] : 0;
    MultiMaster* m = W.master;
    m->server = *(void**)(W.lmi + LMI_SERVER);
    if (chance(50)) memcpy(m->proposal, c->proposal, 0x3c);
    else random_proposal(m->proposal);
    m->glint_frame = irange(0, 7);
    m->config_time = m->server && chance(50) ? (int32_t)(rnd() & 0x7fffff) : 0;     // (the host's: a client never sets it)
    m->approve_time = chance(40) ? 0 : (int32_t)(rnd() & 0x7fffff);
    m->last_state = chance(50) ? c->state : irange(0, 13);
    m->done = (uint8_t)(chance(10) ? 1 : 0);
    m->approved = (uint8_t)(rnd() & 1);
    m->config_dirty = (uint8_t)(rnd() & 1);
    MultiCarChooser* cc = W.carch;
    cc->arrows = (uint8_t)(rnd() & 1); cc->garage = (uint8_t)(rnd() & 1);
    *(int32_t*)(cc->viewer + CV_CAR) = irange(0, 7);
    MultiTrackChooser* t = W.track;
    t->host = (uint8_t)(rnd() & 1);
    t->track = irange(0, t->count > 0 ? t->count - 1 : 0);
    t->saved = irange(-1, 6);
    MultiRaceCfg* g = W.cfg;
    if (g) {
        g->realism = irange(0, 2); g->time_of_day = irange(0, 3); g->weather = irange(0, 3); g->race_type = irange(0, 3);
        g->laps = irange(0, 9); g->strength = irange(0, 2); g->damage = (uint8_t)(rnd() & 1); g->reversed = (uint8_t)(rnd() & 1);
        g->opponents = irange(0, 6); g->iroc = (uint8_t)(rnd() & 1);
        g->opp_f = chance(80) ? (float)irange(0, 6) : range(0.0f, 6.0f);  // NumberString's range
        g->type_f = chance(80) ? (float)irange(1, 3) : range(1.0f, 3.0f);
    }
    for (int k = 0; k < 16; k++) W.ints[k] = irange(-1, 4);
    // the stubs' world
    HS->time = (int32_t)(chance(10) ? rnd() & 0x7fffffff : rnd() & 0x7fffff);
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->grab_fail_at = chance(20) ? irange(1, 6) : -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->script_n = HS->script_pos = 0;
    HS->opt_seed = rnd();
}
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x125, 0x127, 0x123, 0x124, 0x128, 'q', 'a', 'Z', '5', 'h', ' ', 9, 0x141};
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
        {0x004f72e4, 0x004f8c00},                               // the menus' strings (msched's: 0x4f83d0 ..)
        {0x004e4db8, 0x004e4dbc},
        {0x004e52e4, 0x004e52e8},                               // HackEnabled (the round's)
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
    {0x0057a0f0, 1, 0x0057a0c0, 0x004f8514},
    {0x0057a000, 1, 0x00579fd8, 0x004f8538}, {0x0057a000, 2, 0x0057a138, 0x004f8554},
    {0x0057a2cc, 1, 0x0057a208, 0x004f85f0}, {0x0057a2cc, 2, 0x0057a270, 0x004f8608}, {0x0057a2cc, 4, 0x00579f80, 0x004f8620},
    {0x0057a2cc, 8, 0x0057a260, 0x004f8638}, {0x0057a2cc, 0x10, 0x00579f70, 0x004f864c}, {0x0057a2cc, 0x20, 0x0057a218, 0x004f8660},
    {0x0057a2cc, 0x40, 0x0057a170, 0x004f8684},
    {0x0057a154, 1, 0x0057a088, 0x004f86d0}, {0x0057a154, 2, 0x0057a2c0, 0x004f86f0},
    {0x00579f6c, 1, 0x0057a2a0, 0x004f8718},
    {0x0057a008, 1, 0x00579f50, 0x004f8794}, {0x0057a008, 2, 0x00579f10, 0x004f87a0}, {0x0057a008, 4, 0x0057a280, 0x004f87ac},
    {0x0057a008, 8, 0x0057a290, 0x004f87b8}, {0x0057a008, 0x10, 0x00579fc8, 0x004f87c8}, {0x0057a008, 0x20, 0x0057a1c8, 0x004f87d4},
    {0x0057a008, 0x40, 0x0057a160, 0x004f87e0}, {0x0057a008, 0x80, 0x0057a1b0, 0x004f87ec},
    {0x0057a0bc, 1, 0x00579f60, 0x004f87fc}, {0x0057a0bc, 2, 0x0057a190, 0x004f880c}, {0x0057a0bc, 4, 0x00579fb8, 0x004f8818},
    {0x0057a0bc, 8, 0x0057a1a0, 0x004f8824}, {0x0057a0bc, 0x10, 0x0057a2b0, 0x004f8834}, {0x0057a0bc, 0x20, 0x0057a0a8, 0x004f8844},
    {0x0057a0bc, 0x40, 0x00579ff0, 0x004f8854},
    {0x0057a2d0, 1, 0x0057a1f8, 0x004f8984}, {0x0057a2d0, 2, 0x0057a238, 0x004f8994},
    {0x0057a1e4, 1, 0x0057a0d0, 0x004f89e8}, {0x0057a1e4, 2, 0x0057a180, 0x004f89f8}, {0x0057a1e4, 4, 0x0057a108, 0x004f8a08},
    {0x0057a1e4, 8, 0x0057a038, 0x004f8a1c},
    {0x0057a230, 1, 0x0057a098, 0x004f8a3c}, {0x0057a230, 2, 0x0057a0e0, 0x004f8a5c}, {0x0057a230, 4, 0x0057a148, 0x004f8a80},
    {0x0057a230, 8, 0x00579f98, 0x004f8aa8}, {0x0057a230, 0x10, 0x0057a128, 0x004f8ac4},
    {0x0057a134, 1, 0x00579f20, 0x004f8ad8}, {0x0057a134, 2, 0x0057a1d8, 0x004f8ae0},
};
static void randomize_statics() {
    uint32_t last = 0;
    bool constructed = false;
    for (const auto& s : k_statics) {
        if (s.guard != last) {                               // one choice per guard
            last = s.guard;
            constructed = chance(60);
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
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static uint32_t canvas_arg() {
    if (chance(60) && W.win) return U32(&W.win->canvas);
    return U32(W.screen);
}
static void* self_for(const char* cls) {
    if (!strcmp(cls, "ChatControl")) return W.chat;
    if (!strcmp(cls, "ChatWindow")) return &W.chat->window;
    if (!strcmp(cls, "MultiMaster")) return W.master;
    if (!strcmp(cls, "MultiCarChooser")) return W.carch;
    if (!strcmp(cls, "MultiTrackChooser")) return W.track;
    if (!strcmp(cls, "MultiRaceCfg")) return W.cfg;
    if (!strcmp(cls, "MultiRaceInfo")) return W.info;
    if (!strcmp(cls, "LiveMultiKiller")) return W.killer;
    return 0;
}
static void random_gi() {
    uint8_t* g = W.gi;
    fill_random(g, 0x80);
    *(void**)(g + MGI_SOCKET) = (void*)(uintptr_t)(0x50c0000u + (rnd() & 0xfff0));
    g[MGI_HOST] = (uint8_t)(chance(50) ? 1 : 0);
    g[MGI_BY_NAME] = (uint8_t)(rnd() & 1);
    snprintf((char*)g + MGI_PASSWORD, 0x12, "%s", chance(40) ? "" : "secret");
    snprintf((char*)g + MGI_NAME, 0x20, "%s", chance(50) ? "Player's game" : "host.local");
    memset(W.lmi2, 0, 0x40);
    W.lmi2[LMI_LIVE] = (uint8_t)(chance(80) ? 0 : 1);
    *(void**)(W.lmi2 + LMI_CLIENT) = g_client;
    *(void**)(W.lmi2 + LMI_SERVER) = chance(50) ? (void*)g_server : 0;
    *(void**)(W.lmi2 + LMI_SM) = g_sm;
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
    random_proposal(W.prop);
    const bool cdecl_static = !f.fast;                       // a static member or a free function
    if (cls[0] && !cdecl_static) {
        if (!strcmp(meth, cls)) {                            // a constructor: a fresh random block
            w[0] = U32(W.scratch);
            if (!strcmp(cls, "ChatControl") || !strcmp(cls, "ChatWindow")) a[0] = U32(g_client);
            else if (!strcmp(cls, "MultiTrackChooser")) a[0] = (uint32_t)(chance(50) ? 1 : 0) | (rnd() & 0xffffff00u);
            else if (!strcmp(cls, "MultiMaster")) a[0] = U32(W.lmi);
            return true;
        }
        void* self = self_for(cls);
        if (!self) return false;
        w[0] = U32(self);
        if (strstr(meth, "deleting destructor")) a[0] = rnd() & 3;
        else if (!strcmp(meth, "Draw")) a[0] = canvas_arg();
        else if (!strcmp(meth, "Callback")) { a[0] = (uint32_t)(chance(85) ? irange(0, 4) : irange(-1, 6)); a[1] = chance(50) ? 0 : U32(&W.ints[0]); }
        else if (strstr(meth, "Mouse")) { a[0] = (uint32_t)irange(-20, 700); a[1] = (uint32_t)irange(0x100, 0x1b0); }
        else if (!strcmp(meth, "set_arrows") || !strcmp(meth, "set_garage")) a[0] = (uint32_t)(rnd() & 1) | (rnd() & 0xffffff00u);
        else if (!strcmp(meth, "fillout") || !strcmp(meth, "set_text_from_proposal") || !strcmp(meth, "set_track(NetProposal)")) a[0] = U32(W.prop);
        else if (!strcmp(meth, "set_text_from_race")) a[0] = U32(W.prop + NP_RACE);
        else if (!strcmp(meth, "set_track(int)")) a[0] = (uint32_t)irange(0, W.track->count > 0 ? W.track->count - 1 : 0);
        else if (!strcmp(meth, "set_race_number")) { a[0] = (uint32_t)irange(0, 14); a[1] = (uint32_t)irange(0, 14); }
        else if (!strcmp(meth, "update_race")) { a[0] = U32(W.prop + NP_RACE); a[1] = (uint32_t)(chance(50) ? 0 : rnd() & 0xff) | (rnd() & 0xffffff00u); }
        return true;
    }
#define IS(n) (!strcmp(nm, n))
    if (IS("break_chatline")) { random_line(W.line, 0x1f0); a[0] = U32(W.line); }
    else if (IS("NumberString")) a[0] = (uint32_t)irange(0, 14);
    else if (IS("MultiRaceInfo::UpdateProposal")) a[0] = U32(W.prop);
    else if (IS("UI_SLIDER")) {
        a[0] = U32(W.scratch); a[1] = rnd(); a[2] = rnd(); a[3] = rnd(); a[4] = rnd(); a[5] = U32(&W.ints[3]);
        a[6] = fbits_(range(0, 2)); a[7] = fbits_(range(2, 9)); a[8] = (uint32_t)irange(-3, 9);
    }
    else if (IS("MenuMultiScheduler")) { random_gi(); a[0] = U32(W.lmi2); a[1] = U32(W.gi); }
    else if (IS("MultiDo")) a[0] = U32(W.lmi);
    else if (IS("create_server")) { random_gi(); a[0] = U32(g_sm); a[1] = U32(W.gi); }
    else if (IS("create_client")) { random_gi(); a[0] = U32(W.gi); a[1] = U32(g_sm); }
    else if (strstr(nm, "Tick") || strstr(nm, "Idle")) a[0] = U32(&W.ints[irange(0, 15)]);
    else a[0] = rnd();                                          // the static callbacks
#undef IS
    return true;
}

#if MENU_FIXES
// ---- the fixes, each on its bad case (directed_fix_tests) -----------------------------------------------------------------
// Built with /DVP_MENU_FIXES. Each fix's bad case runs on the rewrite alone (the original would overrun its frame or the
// object there, or call through a stray pointer -- this program's stack among what it would take), from a world set up for
// it (fx_reset, then the case): it must return cleanly (no fault, the bytes popped, ebx / esi / edi / ebp kept), write
// nothing outside what it's given (every other byte of .data/.bss/.idata and the arena compared with before, where the
// function doesn't run a dialog) and give what the fix promises (a text cut to its field, the track's own name, no call
// made). Where it's harmless (an overrun of an object in the arena) the original is run on the case too, to show it is a
// bad case. Each fix's boundary case (the longest input that fits, or the last one in range) runs on the original and the
// rewrite from the same state and must give the same memory, call logs and result. VP_FIXTEST=n runs group n alone (to
// watch one fix's test fail on a build without the fixes: /DVP_MENU_FIXES /DVP_FAITHFUL).
static int g_fx_bad, g_fx_n, g_fx_same_n;
static const Ent& fx_fn(const char* name) {
    for (int i = 0; i < g_nfns; i++)
        if (!strcmp(g_fns[i].name, name)) return g_fns[i];
    printf("  fix test: %s isn't listed\n", name);
    fflush(stdout);
    ExitProcess(4);
}
struct Span { const void* p; uint32_t n; };
// the first byte changed since `before` outside the spans (the stubs' state block aside); 0 if none
static uint32_t fx_outside(const Mem& before, const std::vector<Span>& ok) {
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
        if (before.arena[i] != g_arena[i] && !in(g_arena + i)) return U32(g_arena + i);
    return 0;
}
// the running window and its widgets (UIShowGroup / UIHideGroup / UICustomControl::Dirty write them)
static std::vector<Span> fx_win(std::initializer_list<Span> more) {
    std::vector<Span> v(more);
    v.push_back({W.win, (uint32_t)sizeof(WidgetWindow)});
    for (Widget* w : W.widgets) v.push_back({w, widget_size(w)});
    return v;
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
    if (r) printf(" (fault %d %08x at %08x, popped %u, return %08x, ebx esi edi ebp %08x %08x %08x %08x)", r->fault, r->code, r->eip,
                  r->pops, r->ret, r->regs[0], r->regs[1], r->regs[2], r->regs[3]);
    if (where) printf(" (wrote %08x)", where);
    printf("\n");
}
static int fx_count(uint32_t tag) {
    int n = 0;
    for (uint32_t i = 0; i < g_log.n && i < LOG_MAX; i++) n += g_log.w[i] == tag;
    return n;
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
    fx_check(same, m, &ro, where);
}
// an Xlator (a function-local one: its guard's bit set; a static one: guard 0) with the text given, fresh
static void fx_xl(uint32_t guard, uint8_t bit, uint32_t xl, const char* text) {
    if (guard) UI_G8(guard) = (uint8_t)(UI_G8(guard) | bit);
    UI_GU32(xl + 4) = U32(text);
    UI_GU32(xl + 8) = UI_GU32(S_XLATOR_COOKIE);
}
// NumberString's Xlators all constructed, their texts the one given (or their keys' last parts)
static void fx_numbers(const char* text) {
    UI_G8(S_NUM_ONCE) = 0xff;
    UI_G8(S_NUM_ONCE2) = (uint8_t)(UI_G8(S_NUM_ONCE2) | 0x7f);
    for (int k = 0; k < 15; k++) {
        static const uint32_t keys[15] = {0x004f8794, 0x004f87a0, 0x004f87ac, 0x004f87b8, 0x004f87c8, 0x004f87d4, 0x004f87e0, 0x004f87ec,
                                          0x004f87fc, 0x004f880c, 0x004f8818, 0x004f8824, 0x004f8834, 0x004f8844, 0x004f8854};
        UI_GU32(k_number_xl[k]) = keys[k];
        fx_xl(0, 0, k_number_xl[k], text ? text : (const char*)(uintptr_t)xl_text(keys[k]));
    }
}
// a string of n c's and then tail (in one of eight buffers of this program's)
static char g_fx_s[8][0x400];
static const char* mk(int k, char c, int n, const char* tail = "") {
    memset(g_fx_s[k], c, (size_t)n);
    strcpy(g_fx_s[k] + n, tail);
    return g_fx_s[k];
}
// what the game's sprintf would print, its first `keep` characters
static std::string fx_expect(uint32_t keep, const char* fmt, ...) {
    static char t[0x2000];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(t, fmt, ap);
    va_end(ap);
    if (strlen(t) > keep) t[keep] = 0;
    return t;
}
// a callee caught for a test: a jump to the harness's own (or a rewrite), the original's bytes put back after
struct FxPatch {
    uint32_t at;
    uint8_t b[5];
    FxPatch(uint32_t a, void* to) : at(a) { memcpy(b, (const void*)(uintptr_t)a, 5); patch_jmp(a, to); }
    ~FxPatch() { memcpy((void*)(uintptr_t)at, b, 5); }
};
static char g_fx_box[0x400];
static void __cdecl fx_drat_box(const char*, const char* text, uint32_t) { L('FXDB'); strncpy(g_fx_box, text, 0x3ff); }
// the tests' world: pristine, a deterministic state, the overrides off, the statics the code follows
static const char* g_fx_names[64];
static char g_fx_names_buf[64][32];
static void fx_reset(uint32_t seed) {
    g_rng = seed * 2654435761u | 1;
    mem_load(g_pristine);
    randomize_world();
    randomize_statics();
    g_fx_th_set = false; g_fx_tn = 0; g_fx_tfn = g_fx_car = g_fx_carf = g_fx_str = 0;
    g_fx_lap[0] = g_fx_rsml[0] = g_fx_box[0] = 0;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_G8(S_EXIT) = 0;
    HS->frames = 0; HS->frame_limit = 40; HS->script_n = HS->script_pos = 0;
    fx_numbers(0);
}
// the add-on tracks: n names of the form "addon_track_NN" (14 characters), or "trk_NN" (6)
static void fx_tracks(bool long_names) {
    for (int i = 0; i < 64; i++) {
        sprintf(g_fx_names_buf[i], long_names ? "addon_track_%02d" : "trk_%02d", i);
        g_fx_names[i] = g_fx_names_buf[i];
    }
    g_fx_tn = g_fx_names;
}
// the chooser as its fixed constructor leaves it for `count` tracks of the names above
static void fx_chooser(int32_t count) {
    MultiTrackChooser* t = W.track;
    t->count = count;
    for (int i = 0; i < 16; i++) {
        strncpy(t->names[i], g_fx_tn[i], 12);
        t->names[i][12] = 0;
    }
}

static int directed_fix_tests() {
    char m[256];
    const char* only_fx = getenv("VP_FIXTEST");
    auto fx_on = [&](int g) { return !only_fx || atoi(only_fx) == g; };
    // ---- 1 MultiTrackChooser's constructor: 28 tracks (an add-on table), names of 14 characters -----------------------------
    if (fx_on(1)) {
        const Ent& f = fx_fn("MultiTrackChooser::MultiTrackChooser");
        fx_reset(0xa100);
        fx_tracks(true);
        HS->ntracks = 30;
        memset(W.scratch, 0xa5, 0x800);
        mem_save(g_snap);
        Result r = fx_run(f, true, {U32(W.scratch), 0, 1});
        const MultiTrackChooser* t = (const MultiTrackChooser*)W.scratch;
        bool names = true;
        for (int i = 0; i < 16; i++) names &= !strncmp(t->names[i], g_fx_names[i], 12) && t->names[i][12] == 0;
        const std::vector<Span> obj = {{W.scratch, (uint32_t)sizeof(MultiTrackChooser)}, {(const void*)(uintptr_t)S_TRACKCH_G, 4}};
        const uint32_t out = fx_outside(g_snap, obj);
        fx_check(fx_clean(f, r) && r.ret == U32(W.scratch) && t->count == 28 && t->host == 1 && t->stamp == 0 && names &&
                 fx_count('GTNM') == 16 && out == 0,
                 "MultiTrackChooser, 28 tracks of 14 characters: clean, the first 16 names cut to 12, the host flag kept, nothing past the object",
                 &r, out);
        mem_load(g_snap);
        r = fx_run(f, false, {U32(W.scratch), 0, 1});
        fx_check(r.fault || fx_outside(g_snap, obj) != 0,
                 "MultiTrackChooser, 28 tracks: the original (for comparison) writes past the object");
        fx_reset(0xa101);
        fx_tracks(false);
        HS->ntracks = 18;
        memset(W.scratch, 0xa5, 0x800);
        fx_same(f, {U32(W.scratch), 0, 1}, "MultiTrackChooser, 16 tracks of 6 characters");
        fx_reset(0xa102);
        HS->ntracks = 8;
        memset(W.scratch, 0xa5, 0x800);
        fx_same(f, {U32(W.scratch), 0, 0}, "MultiTrackChooser, the stock 6 tracks (an 11-character name among them)");
    }
    // ---- 2 MultiTrackChooser::set_track: a track past the first 16, a 12-character name, a track this machine doesn't have --
    if (fx_on(2)) {
        const Ent& f = fx_fn("MultiTrackChooser::set_track(int)");
        static const int32_t ts[4] = {20, 5, 28, -1};
        for (int32_t tr : ts) {
            fx_reset(0xa200);
            fx_tracks(true);
            fx_chooser(28);
            W.track->stamp = 0;
            const Result r = fx_run(f, true, {U32(W.track), 0, (uint32_t)tr});
            const bool have = tr >= 0 && tr < 28;
            char want[64];
            sprintf(want, "%s.stp", have ? g_fx_names[tr] : "");
            sprintf(m, "set_track(%d) of 28 (names of 14 characters): clean, %s", tr, have ? "the track's whole name's picture" : "no picture, none loaded");
            fx_check(fx_clean(f, r) && W.track->track == tr &&
                     (have ? W.track->stamp == (void*)fake_for(want, 'STMP') && fx_count('GSTP') == 1 : W.track->stamp == 0 && fx_count('GSTP') == 0),
                     m, &r);
        }
        fx_reset(0xa201);
        fx_same(f, {U32(W.track), 0, 5}, "set_track(5) of the stock 6 (an 11-character name)");
        fx_reset(0xa202);
        fx_tracks(false);
        fx_chooser(16);
        fx_same(f, {U32(W.track), 0, 15}, "set_track(15) of 16");
    }
    // ---- 3 MultiRaceCfg::Callback (2, the race type): the laps of a track past the first 16, of one not here ---------------
    if (fx_on(3)) {
        const Ent& f = fx_fn("MultiRaceCfg::Callback");
        for (int k = 0; k < 2; k++) {
            fx_reset(0xa300 + k);
            fx_tracks(true);
            fx_chooser(28);
            W.track->track = k ? 28 : 20;
            W.cfg->laps = 777;
            W.cfg->type_f = 2.0f;
            const Result r = fx_run(f, true, {U32(W.cfg), 0, 2, U32(&W.ints[0])});
            sprintf(m, "MultiRaceCfg::Callback(2), track %d of 28: clean, %s", k ? 28 : 20,
                    k ? "the laps left as they were, GetLapCountFromType not called" : "the laps of the track's whole name");
            fx_check(fx_clean(f, r) && (k ? W.cfg->laps == 777 && fx_count('GLAP') == 0 && g_fx_lap[0] == 0
                                          : !strcmp(g_fx_lap, g_fx_names[20]) && W.cfg->laps == HS->laps + 2),
                     m, &r);
        }
        fx_reset(0xa302);
        W.track->track = 5;
        fx_same(f, {U32(W.cfg), 0, 2, U32(&W.ints[0])}, "MultiRaceCfg::Callback(2), the stock track 5");
    }
    // ---- 4 NumberString outside 0..14 -------------------------------------------------------------------------------------------
    if (fx_on(4)) {
        const Ent& f = fx_fn("NumberString");
        static const int32_t ns[4] = {15, -1, 0x7fffffff, (int32_t)0x80000000};
        for (int32_t n : ns) {
            fx_reset(0xa400);
            mem_save(g_snap);
            const Result r = fx_run(f, true, {(uint32_t)n});
            const uint32_t out = fx_outside(g_snap, {});
            sprintf(m, "NumberString(%d): clean, \"\"", n);
            fx_check(fx_clean(f, r) && r.ret == S_EMPTY_STR && out == 0, m, &r, out);
        }
        for (int32_t n = 0; n <= 14; n += 14) {
            fx_reset(0xa401);
            sprintf(m, "NumberString(%d)", n);
            fx_same(f, {(uint32_t)n}, m);
        }
    }
    // ---- 5 MultiMaster::Update: the client's state past 13 (the race's) or negative ------------------------------------------------
    if (fx_on(5)) {
        const Ent& f = fx_fn("MultiMaster::Update");
        static const int32_t st[5][2] = {{12, 14}, {14, 0x15}, {13, 0xe}, {9, -5}, {12, 100}};
        for (auto& s2 : st) {
            fx_reset(0xa500);
            MultiMaster* mm = W.master;
            mm->last_state = s2[0];
            g_client->state = s2[1];
            mm->done = 0;
            mm->config_time = 0;
            const Result r = fx_run(f, true, {U32(mm), 0});
            const bool past = s2[1] > 13;
            sprintf(m, "MultiMaster::Update, state %d after %d: clean, %s", s2[1], s2[0],
                    past ? "the lobby done (as car get)" : "as state 0 (sf_hang's panic)");
            fx_check(fx_clean(f, r) && mm->last_state == s2[1] && (past ? mm->done == 1 : fx_count('PANC') == 1), m, &r);
        }
        fx_reset(0xa501);
        W.master->last_state = 12; g_client->state = 13; W.master->done = 0;
        fx_same(f, {U32(W.master), 0}, "MultiMaster::Update, state 13 (car get) after 12");
    }
    // ---- 6 MultiDo: the client already past car get (14) when the lobby sees it ----------------------------------------------
    if (fx_on(6)) {
        const Ent& f = fx_fn("MultiDo");
        FxPatch up(0x00488610, fx_fn("MultiMaster::Update").fn);       // (the master's Update is the rewrite, as in the DLL)
        for (int32_t s2 = 14; s2 <= 0x15; s2 += 7) {
            fx_reset(0xa600);
            g_client->state = s2;
            HS->frame_limit = 200;
            const Result r = fx_run(f, true, {U32(W.lmi)});
            sprintf(m, "MultiDo, the client at state %d: clean, 1 (the race follows)", s2);
            fx_check(fx_clean(f, r) && r.ret == 1 && fx_count('WDOG') == 0, m, &r);
        }
        fx_reset(0xa601);
        g_client->state = 13;
        HS->frame_limit = 200;
        fx_same(f, {U32(W.lmi)}, "MultiDo, the client at car get (13)");
        fx_reset(0xa602);
        g_client->state = 12;
        HS->script[0][0] = OP_NOP; HS->script[1][0] = OP_KEY; HS->script[1][1] = 0x1b; HS->script_n = 2;   // Escape
        fx_same(f, {U32(W.lmi)}, "MultiDo, the client at car waiting (12), Escape");
    }
    // ---- 7 ChatControl::Draw / MouseMove: a text height of -3 ------------------------------------------------------------------
    if (fx_on(7)) {
        const Ent& dr = fx_fn("ChatControl::Draw");
        const Ent& mv = fx_fn("ChatControl::MouseMove");
        fx_reset(0xa700);
        g_fx_th_set = true; g_fx_th = -3;
        Result r = fx_run(dr, true, {U32(W.chat), 0, U32(W.screen)});
        fx_check(fx_clean(dr, r) && fx_count('FCNT') == 0, "ChatControl::Draw, a text height of -3: clean, no rows", &r);
        fx_reset(0xa701);
        g_fx_th_set = true; g_fx_th = -3;
        W.chat->dragging = 1;
        HS->nusers = 3;
        r = fx_run(mv, true, {U32(W.chat), 0, 30, 0x150});
        fx_check(fx_clean(mv, r) && W.chat->sel == 0, "ChatControl::MouseMove, a text height of -3: clean, the first row picked", &r);
        fx_reset(0xa702);
        fx_same(dr, {U32(W.chat), 0, U32(W.screen)}, "ChatControl::Draw, a text height of 8");
        fx_reset(0xa703);
        W.chat->dragging = 1;
        fx_same(mv, {U32(W.chat), 0, 30, 0x150}, "ChatControl::MouseMove, a text height of 8");
    }
    // ---- 8 ChatWindow::Draw: a name padding past the line's room ----------------------------------------------------------------
    if (fx_on(8)) {
        const Ent& f = fx_fn("ChatWindow::Draw");
        static const int32_t ws[4] = {0x200, -0x200, (int32_t)0x80000000, 0x7fffffff};
        for (int32_t w : ws) {
            fx_reset(0xa800);
            HS->maxname = w;
            HS->scroll_on = 1;
            const Result r = fx_run(f, true, {U32(&W.chat->window), 0, U32(W.screen)});
            sprintf(m, "ChatWindow::Draw, a name padding of %d: clean", w);
            fx_check(fx_clean(f, r), m, &r);
        }
        fx_reset(0xa801);
        HS->maxname = 0xc8;                                             // 0xc8 + ": " + 49 = 251 characters: the most that fit
        HS->scroll_on = 1;
        for (int i = 0; i < 8; i++) { memset(g_client->ring[i].text, 'w', 0x31); g_client->ring[i].text[0x31] = 0; }
        fx_same(f, {U32(&W.chat->window), 0, U32(W.screen)}, "ChatWindow::Draw, lines of 251 characters");
    }
    // ---- 9 MultiMaster::race: a car name of 31 characters ("<car>.car") ------------------------------------------------------------
    if (fx_on(9)) {
        const Ent& f = fx_fn("MultiMaster::race");
        fx_reset(0xa900);
        g_client->state = 0xb;
        g_fx_car = mk(0, 'c', 31);
        const Result r = fx_run(f, true, {U32(W.master), 0});
        fx_check(fx_clean(f, r) && fx_expect(0x400, "%s.car", g_fx_s[0]) == g_fx_rsml,
                 "MultiMaster::race, a 31-character car: clean, \"<car>.car\" loaded by its whole name", &r);
        fx_reset(0xa901);
        g_client->state = 0xb;
        g_fx_car = mk(0, 'c', 27);
        fx_same(f, {U32(W.master), 0}, "MultiMaster::race, a 27-character car");
    }
    // ---- 10 MultiCarChooser::update_car: a friendly name over 33 characters -----------------------------------------------------
    if (fx_on(10)) {
        const Ent& f = fx_fn("MultiCarChooser::update_car");
        fx_reset(0xaa00);
        g_fx_carf = mk(0, 'f', 50);
        mem_save(g_snap);
        const Result r = fx_run(f, true, {U32(W.carch), 0});
        const uint32_t out = fx_outside(g_snap, {{W.carch->name, (uint32_t)sizeof W.carch->name}});
        fx_check(fx_clean(f, r) && fx_expect(33, "%s", g_fx_s[0]) == W.carch->name && out == 0,
                 "MultiCarChooser::update_car, a 50-character name: clean, 33 kept, the CarViewer3D untouched", &r, out);
        fx_reset(0xaa01);
        g_fx_carf = mk(0, 'f', 33);
        fx_same(f, {U32(W.carch), 0}, "MultiCarChooser::update_car, a 33-character name");
    }
    // ---- 11 MultiTrackChooser::set_text_from_race: a long name and the reversed text; a track not here --------------------------
    if (fx_on(11)) {
        const Ent& f = fx_fn("MultiTrackChooser::set_text_from_race");
        uint8_t* ri = W.prop + NP_RACE;
        fx_reset(0xab00);
        *(int32_t*)(ri + NRI_TRACK) = 2; ri[NRI_REVERSED] = 1;
        g_fx_tfn = mk(0, 'F', 70);
        fx_xl(0, 0, 0x00579f40, "Reversed");
        mem_save(g_snap);
        Result r = fx_run(f, true, {U32(W.track), 0, U32(ri)});
        uint32_t out = fx_outside(g_snap, {{W.track->text, (uint32_t)sizeof W.track->text}});
        fx_check(fx_clean(f, r) && fx_expect(63, "%s - %s", g_fx_s[0], "Reversed") == W.track->text && out == 0,
                 "set_text_from_race, a 70-character name, reversed: clean, 63 kept, the race number's text untouched", &r, out);
        fx_reset(0xab01);
        *(int32_t*)(ri + NRI_TRACK) = W.track->count;
        mem_save(g_snap);
        r = fx_run(f, true, {U32(W.track), 0, U32(ri)});
        out = fx_outside(g_snap, {{W.track->text, (uint32_t)sizeof W.track->text}});
        fx_check(fx_clean(f, r) && W.track->text[0] == 0 && fx_count('GTFN') == 0 && out == 0,
                 "set_text_from_race, a track this machine doesn't have: clean, no name, GetTrackFriendlyName not called", &r, out);
        fx_reset(0xab02);
        *(int32_t*)(ri + NRI_TRACK) = 2; ri[NRI_REVERSED] = 1;
        g_fx_tfn = mk(0, 'F', 63 - 3 - 8);
        fx_xl(0, 0, 0x00579f40, "Reversed");
        fx_same(f, {U32(W.track), 0, U32(ri)}, "set_text_from_race, a text of 63 characters");
        fx_reset(0xab03);
        *(int32_t*)(ri + NRI_TRACK) = W.track->count - 1; ri[NRI_REVERSED] = 0;
        fx_same(f, {U32(W.track), 0, U32(ri)}, "set_text_from_race, the last track");
    }
    // ---- 12 MultiTrackChooser::set_race_number: long number translations -----------------------------------------------------
    if (fx_on(12)) {
        const Ent& f = fx_fn("MultiTrackChooser::set_race_number");
        fx_reset(0xac00);
        fx_numbers(mk(0, 'n', 30));
        mem_save(g_snap);
        const Result r = fx_run(f, true, {U32(W.track), 0, 3, 5});
        const uint32_t out = fx_outside(g_snap, {{W.track->race_text, (uint32_t)sizeof W.track->race_text}});
        fx_check(fx_clean(f, r) && fx_expect(35, "XXX Race %s of %s XXX", g_fx_s[0], g_fx_s[0]) == W.track->race_text && out == 0,
                 "set_race_number, 30-character numbers: clean, 35 kept, the count and track untouched", &r, out);
        fx_reset(0xac01);
        fx_numbers(mk(0, 'n', 9));
        fx_same(f, {U32(W.track), 0, 3, 5}, "set_race_number, a text of 35 characters");
    }
    // ---- 13 MultiRaceCfg: the AI cars' and the laps' texts with long translations ------------------------------------------------
    if (fx_on(13)) {
        const Ent& ct = fx_fn("MultiRaceCfg::MultiRaceCfg");
        const Ent& cb = fx_fn("MultiRaceCfg::Callback");
        fx_reset(0xad00);
        fx_xl(0, 0, 0x0057a020, mk(0, 'A', 40));
        memset(W.scratch, 0xa5, 0x800);
        mem_save(g_snap);
        Result r = fx_run(ct, true, {U32(W.scratch), 0});
        const MultiRaceCfg* c = (const MultiRaceCfg*)W.scratch;
        uint32_t out = fx_outside(g_snap, {{W.scratch, 0x68}, {(const void*)(uintptr_t)S_RACECFG_G, 4}});
        fx_check(fx_clean(ct, r) && strlen(c->opp_text) == 15 && !strncmp(c->opp_text, g_fx_s[0], 15) && out == 0,
                 "MultiRaceCfg's constructor, a 40-character translation: clean, the AI cars' text 15, the laps' untouched", &r, out);
        fx_reset(0xad01);
        fx_xl(S_RACECFG_ONCE, 1, 0x0057a1f8, mk(0, 'A', 40));
        fx_xl(S_RACECFG_ONCE, 2, 0x0057a238, mk(1, 'L', 40));
        W.cfg->opp_f = 3.0f;
        mem_save(g_snap);
        r = fx_run(cb, true, {U32(W.cfg), 0, 1, U32(&W.ints[0])});
        out = fx_outside(g_snap, fx_win({{(const void*)&W.cfg->opponents, 4}, {W.cfg->opp_text, (uint32_t)sizeof W.cfg->opp_text}}));
        fx_check(fx_clean(cb, r) && strlen(W.cfg->opp_text) == 15 && out == 0,
                 "MultiRaceCfg::Callback(1), a 40-character translation: clean, 15 kept, the laps' text untouched", &r, out);
        fx_reset(0xad02);
        fx_xl(S_RACECFG_ONCE, 1, 0x0057a1f8, mk(0, 'A', 40));
        fx_xl(S_RACECFG_ONCE, 2, 0x0057a238, mk(1, 'L', 40));
        W.cfg->type_f = 2.0f;
        mem_save(g_snap);
        r = fx_run(cb, true, {U32(W.cfg), 0, 2, U32(&W.ints[0])});
        out = fx_outside(g_snap, {{(const void*)&W.cfg->race_type, 4}, {(const void*)&W.cfg->laps, 4},
                                  {W.cfg->laps_text, (uint32_t)sizeof W.cfg->laps_text}});
        fx_check(fx_clean(cb, r) && strlen(W.cfg->laps_text) == 23 && out == 0,
                 "MultiRaceCfg::Callback(2), a 40-character translation: clean, 23 kept, the groups untouched", &r, out);
        // the boundaries: "<12>: Three" (15), "<17>: <2 digits>" (23)
        fx_reset(0xad03);
        fx_numbers("Three");
        fx_xl(0, 0, 0x0057a020, mk(0, 'A', 15 - 2 - 5));
        memset(W.scratch, 0xa5, 0x800);
        fx_same(ct, {U32(W.scratch), 0}, "MultiRaceCfg's constructor, an AI cars' text that fits");
        fx_reset(0xad04);
        fx_xl(S_RACECFG_ONCE, 2, 0x0057a238, mk(1, 'L', 23 - 2 - 2));      // (laps 10 + 3: two digits)
        W.cfg->type_f = 3.0f;
        HS->laps = 10;
        fx_same(cb, {U32(W.cfg), 0, 2, U32(&W.ints[0])}, "MultiRaceCfg::Callback(2), a laps' text of 23 characters");
    }
    // ---- 14 MultiRaceInfo: the proposal's texts, long ---------------------------------------------------------------------------
    if (fx_on(14)) {
        const Ent& ur = fx_fn("MultiRaceInfo::update_race");
        const Ent& up = fx_fn("MultiRaceInfo::UpdateProposal");
        auto setup = [&](uint32_t seed, int n) {
            fx_reset(seed);
            g_fx_str = mk(0, 'R', n);
            fx_xl(S_RACE_ONCE, 1, 0x0057a0d0, mk(1, 'E', n));
            fx_xl(S_RACE_ONCE, 2, 0x0057a180, mk(1, 'E', n));
            fx_xl(S_RACE_ONCE, 4, 0x0057a108, mk(2, 'P', n));
            fx_xl(S_RACE_ONCE, 8, 0x0057a038, mk(2, 'P', n));
            fx_xl(0, 0, 0x0057a010, mk(3, 'I', n));
            fx_numbers(mk(4, 'N', n));
            uint8_t* ri = W.prop + NP_RACE;
            *(int32_t*)(ri + NRI_COUNT) = 3; *(int32_t*)(ri + NRI_LAPS) = 5; ri[NRI_DAMAGE] = 1;
            W.prop[NP_IROC] = 1;
        };
        const std::vector<Span> fields = fx_win({{W.info->title, 0xa0 - 0x18}});
        FxPatch rw(F_MRI_update_race, ur.fn);                           // (UpdateProposal's update_race the rewrite, as in the DLL)
        setup(0xae00, 40);
        mem_save(g_snap);
        Result r = fx_run(ur, true, {U32(W.info), 0, U32(W.prop + NP_RACE), 1});
        uint32_t out = fx_outside(g_snap, fields);
        bool ok = strlen(W.info->title) == 15 && strlen(W.info->realism) == 15 && strlen(W.info->damage) == 15 &&
                  strlen(W.info->time) == 15 && strlen(W.info->weather) == 15 && strlen(W.info->type) == 23 &&
                  strlen(W.info->count) == 15 && strlen(W.info->strength) == 15;
        fx_check(fx_clean(ur, r) && ok && out == 0, "MultiRaceInfo::update_race, 40-character texts: clean, each held to its field", &r, out);
        setup(0xae01, 40);
        mem_save(g_snap);
        r = fx_run(up, true, {U32(W.prop)});
        out = fx_outside(g_snap, fields);
        fx_check(fx_clean(up, r) && strlen(W.info->title) == 15 && out == 0,
                 "MultiRaceInfo::UpdateProposal, 40-character texts: clean, each held to its field", &r, out);
        setup(0xae02, 15);
        fx_xl(S_RACE_ONCE, 4, 0x0057a108, mk(2, 'P', 23 - 15 - 2 - 1 - 1));
        fx_same(ur, {U32(W.info), 0, U32(W.prop + NP_RACE), 1}, "MultiRaceInfo::update_race, texts that fill their fields");
        setup(0xae03, 15);
        fx_xl(S_RACE_ONCE, 4, 0x0057a108, mk(2, 'P', 23 - 15 - 2 - 1 - 1));
        fx_same(up, {U32(W.prop)}, "MultiRaceInfo::UpdateProposal, texts that fill their fields");
    }
    // ---- 15 MultiRaceInfo::Added: the ghost types' string ---------------------------------------------------------------------
    if (fx_on(15)) {
        const Ent& f = fx_fn("MultiRaceInfo::Added");
        fx_reset(0xaf00);
        fx_xl(S_RACEINFO_ONCE, 1, 0x0057a098, mk(0, 'a', 40));
        fx_xl(S_RACEINFO_ONCE, 2, 0x0057a0e0, mk(1, 'b', 40));
        fx_xl(S_RACEINFO_ONCE, 4, 0x0057a148, mk(2, 'c', 40));
        uint8_t next[0xc];
        memcpy(next, (const void*)(uintptr_t)0x0057a088, 0xc);
        const Result r = fx_run(f, true, {U32(W.info), 0});
        const char* ms = (const char*)(uintptr_t)S_GHOST_MULTI;
        const bool str = !strcmp(ms, mk(5, 'a', 20)) && !strcmp(ms + 21, mk(6, 'b', 20)) && !strcmp(ms + 42, mk(7, 'c', 20)) && ms[63] == 0;
        fx_check(fx_clean(f, r) && str && !memcmp(next, (const void*)(uintptr_t)0x0057a088, 0xc),
                 "MultiRaceInfo::Added, three 40-character ghost types: clean, each cut to 20 in the 64 bytes, the next Xlator untouched", &r);
        fx_reset(0xaf01);
        fx_xl(S_RACEINFO_ONCE, 1, 0x0057a098, mk(0, 'a', 20));
        fx_xl(S_RACEINFO_ONCE, 2, 0x0057a0e0, mk(1, 'b', 20));
        fx_xl(S_RACEINFO_ONCE, 4, 0x0057a148, mk(2, 'c', 20));
        fx_same(f, {U32(W.info), 0}, "MultiRaceInfo::Added, ghost types of 20 characters (the 64 bytes full)");
    }
    // ---- 16 sf_noconn: a long lost-connection text ------------------------------------------------------------------------------
    if (fx_on(16)) {
        const Ent& f = fx_fn("MultiMaster::sf_noconn");
        FxPatch db(F_UIDoDratBox, (void*)&fx_drat_box);
        fx_reset(0xb000);
        g_client->state = 2;
        fx_xl(S_NOCONN_ONCE, 2, 0x0057a2c0, mk(0, 'L', 200));
        const Result r = fx_run(f, true, {U32(W.master), 0});
        fx_check(fx_clean(f, r) && fx_expect(127, "%s\n%s", g_fx_s[0], k_strs[5][2]) == g_fx_box,
                 "sf_noconn, a 200-character translation: clean, the box's text its first 127", &r);
        fx_reset(0xb001);
        g_client->state = 2;
        fx_xl(S_NOCONN_ONCE, 2, 0x0057a2c0, mk(0, 'L', 127 - 1 - (int)strlen(k_strs[5][2])));
        fx_same(f, {U32(W.master), 0}, "sf_noconn, a text of 127 characters");
    }
    // ---- 17 MenuMultiScheduler: a game's name not ending in its 32 bytes ------------------------------------------------------
    if (fx_on(17)) {
        const Ent& f = fx_fn("MenuMultiScheduler");
        auto setup = [&](uint32_t seed, int n) {
            fx_reset(seed);
            random_gi();
            W.gi[MGI_HOST] = 0; W.gi[MGI_BY_NAME] = 0;
            memset(W.gi + MGI_NAME, 'G', (size_t)n); W.gi[MGI_NAME + n] = 0;
            W.lmi2[LMI_LIVE] = 0;
            memset(W.lmi2 + LMI_NAME, 0x5c, 0x30);
            HS->client_ok = 0;
        };
        setup(0xb100, 0x50);
        const Result r = fx_run(f, true, {U32(W.lmi2), U32(W.gi)});
        bool after = true;
        for (int i = 0x30; i < 0x40; i++) after &= W.lmi2[i] == 0x5c;
        fx_check(fx_clean(f, r) && fx_expect(31, "%s", (const char*)W.gi + MGI_NAME) == (const char*)(W.lmi2 + LMI_NAME) && after,
                 "MenuMultiScheduler, an 80-character name: clean, 31 kept, the task id untouched", &r);
        setup(0xb101, 31);
        fx_same(f, {U32(W.lmi2), U32(W.gi)}, "MenuMultiScheduler, a 31-character name");
    }
    // ---- 18 set_arrows / set_garage: a flag other than 0 or 1 -------------------------------------------------------------------
    if (fx_on(18)) {
        for (int k = 0; k < 2; k++) {
            const Ent& f = fx_fn(k ? "MultiCarChooser::set_garage" : "MultiCarChooser::set_arrows");
            fx_reset(0xb200 + k);
            mem_save(g_snap);
            const Result r1 = fx_run(f, true, {U32(W.carch), 0, 1});
            mem_save(g_after);
            mem_load(g_snap);
            const Result r2 = fx_run(f, true, {U32(W.carch), 0, 2});
            uint8_t* flag = k ? (uint8_t*)&W.carch->garage : (uint8_t*)&W.carch->arrows;
            const bool two = *flag == 2;
            *flag = 1;
            const uint32_t where = mem_diff(g_after);
            sprintf(m, "%s(2): clean, shown as for 1 (the flag 2 kept)", f.name);
            fx_check(fx_clean(f, r1) && fx_clean(f, r2) && two && where == 0, m, &r2, where);
            for (uint32_t v = 0; v < 2; v++) {
                fx_reset(0xb202 + k);
                sprintf(m, "%s(%u)", f.name, v);
                fx_same(f, {U32(W.carch), 0, v}, m);
            }
        }
    }
    // ---- 19 MultiTrackChooser::next / prev with no tracks -----------------------------------------------------------------------
    if (fx_on(19)) {
        for (int k = 0; k < 2; k++) {
            const Ent& f = fx_fn(k ? "MultiTrackChooser::prev" : "MultiTrackChooser::next");
            fx_reset(0xb300 + k);
            W.track->count = 0;
            mem_save(g_snap);
            const Result r = fx_run(f, true, {U32(W.track), 0});
            const uint32_t out = fx_outside(g_snap, {});
            sprintf(m, "%s, no tracks: clean, nothing changed, nothing called", f.name);
            fx_check(fx_clean(f, r) && out == 0 && g_log.n == 0, m, &r, out);
            fx_reset(0xb302 + k);
            sprintf(m, "%s, the stock 6 tracks", f.name);
            fx_same(f, {U32(W.track), 0}, m);
        }
    }
    mem_load(g_pristine);
    g_fx_th_set = false; g_fx_tn = 0; g_fx_tfn = g_fx_car = g_fx_carf = g_fx_str = 0;
    printf("fix tests: %s -- %d checks (%d of them original against rewrite), %d failed\n", g_fx_bad ? "FAILED" : "all passed", g_fx_n,
           g_fx_same_n, g_fx_bad);
    return g_fx_bad;
}
#endif

// ---- main -----------------------------------------------------------------------------------------------------------------------
static bool is_modal(const char* nm) {
    return strstr(nm, "MultiDo") || strstr(nm, "MenuMultiScheduler") || strstr(nm, "xit") || strstr(nm, "noconn") ||
           strstr(nm, "::Added") || !strcmp(nm, "MultiMaster::Update");
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
    char* s = strstr(exe, "\\test\\world_menu_sched.cpp");
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
        {F_OptionsGetI, (void*)&stub_OptionsGetI}, {F_OptionsGetB, (void*)&stub_OptionsGetB}, {F_OptionsSetI, (void*)&stub_OptionsSetI},
        {F_OptionsSetB, (void*)&stub_OptionsSetB},
        {F_HackGetCarIndex, (void*)&stub_HackGetCarIndex}, {F_GetCarFileNumber, (void*)&stub_GetCarFileNumber},
        {F_GetTrackCount, (void*)&stub_GetTrackCount}, {F_GetTrackName, (void*)&stub_GetTrackName},
        {F_GetTrackFriendlyName, (void*)&stub_GetTrackFriendlyName}, {F_GetRealismString, (void*)&stub_GetRealismString},
        {F_GetRaceTypeString, (void*)&stub_GetRaceTypeString}, {F_GetWeatherString, (void*)&stub_GetWeatherString},
        {F_GetGameTimeString, (void*)&stub_GetGameTimeString}, {F_GetAIStrengthString, (void*)&stub_GetAIStrengthString},
        {F_GetLapCountFromType, (void*)&stub_GetLapCountFromType},
        {F_ResourceSetMustLoad, (void*)&stub_ResourceSetMustLoad}, {F_ResourceSetUnload, (void*)&stub_ResourceSetUnload},
        {F_MenuDoOptions, (void*)&stub_MenuDoOptions}, {F_MenuEditCar, (void*)&stub_MenuEditCar},
        {F_CarViewer3D_ctor, (void*)&stub_CV_ctor}, {F_CarViewer3D_dtor, (void*)&stub_CV_dtor}, {F_CarViewer3D_Next, (void*)&stub_CV_Next},
        {F_CarViewer3D_Prev, (void*)&stub_CV_Prev}, {F_CarViewer3D_SetCar, (void*)&stub_CV_SetCar},
        {F_CarViewer3D_GetPaintJob, (void*)&stub_CV_GetPaintJob}, {F_CarViewer3D_GetFriendlyName, (void*)&stub_CV_GetFriendlyName},
        {F_CarViewer3D_GetName, (void*)&stub_CV_GetName},
        {F_MultiEnabled, (void*)&stub_MultiEnabled}, {F_MultiResumeChat, (void*)&stub_MultiResumeChat},
        {F_MultiLiveEnd, (void*)&stub_MultiLiveEnd}, {F_LMI_Grab, (void*)&stub_LMI_Grab}, {F_LMI_Release, (void*)&stub_LMI_Release},
        {F_LMI_AmOwner, (void*)&stub_LMI_AmOwner}, {F_SM_Tick, (void*)&stub_SM_Tick},
        {F_SM_SetConnectPassword, (void*)&stub_SM_SetConnectPassword}, {F_CreateSessionMgr, (void*)&stub_CreateSessionMgr},
        {F_CreateRaceClient_rs, (void*)&stub_CreateRaceClient_rs}, {F_CreateRaceClient_name, (void*)&stub_CreateRaceClient_name},
        {F_CreateRaceClient_id, (void*)&stub_CreateRaceClient_id}, {F_CreateRaceServer, (void*)&stub_CreateRaceServer},
        {F_GetClientStatusString, (void*)&stub_GetClientStatusString}, {F_ClientDisconnectString, (void*)&stub_ClientDisconnectString},
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

#if MENU_FIXES
    const int fix_bad = only ? 0 : directed_fix_tests();
#else
    const int fix_bad = 0;
#endif
    static Footprint fp;
    long long checks = 0, poisoned_checks = 0, log_words = 0;
    int differ = 0, fp_bad = 0, faults = 0, pure_n = 0, replay_n = 0, changed_fns = 0, still_fns = 0, bad_fns = 0, skipped = 0;
    long long both_fault = 0;
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
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
                    if (g_snap.arena[i2] != g_arena[i2] && (fp.pure ? !covered(g_arena + i2) : !covered(g_arena + i2))) out = U32(g_arena + i2);
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
        if (trace) printf("%08x %-56s %s%s (%d checks, %d faulted)\n", f.v10, f.name, fn_bad ? "BAD" : "ok", fn_changed ? "" : " (never changed memory)", fn_checks, fn_faults);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    if (tags) tag_print();
    printf("%d functions (%d pure, %d replay_only, %d others), %d listed twice; %lld checks (%lld on poisoned .data), "
           "%lld logged words compared: %d differ, %d footprint violations, %d checks faulted (%lld in both, the same way), %d skipped; "
           "%d functions bad\n", g_nfns, pure_n, replay_n, g_nfns - pure_n - replay_n, dup, checks, poisoned_checks, log_words, differ,
           fp_bad, faults, both_fault, skipped, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    return differ || fp_bad || dup || fix_bad ? 1 : 0;
}
