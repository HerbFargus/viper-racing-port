// world_career_shop.cpp -- M3 UI stage, step U4 (group B): every rewrite of hook/career_shop.cpp (upgrade.obj) and
// hook/career_credits.cpp (career credits.obj, intro credits.obj, intro.obj) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_career_shop.cpp
//        /Fo<dir>\ /Fe<dir>\world_career_shop.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_career_shop.exe [rounds] [seed]        (VP_TRACE=1: one line per function; VP_ONLY=text: those only;
//          VP_DEBUG_WORLD=1: every fault's registers; VP_DEBUG_LOG=1: each round's first logged words)
//     (and with /DVP_CAREER_FIXES: the fix build, below)
//
// Built as test/world_root_race.cpp is (its loader, raw call, memory comparison and stubs are copied here; the stack is
// filled after the _controlfp_s calls): out\race_v10.exe loaded at 0x400000 in a child process with the range reserved,
// the two rewrite files included with PORT_FN redefined to list each function (its v1.0 address, the rewrite, its calling
// convention -- __cdecl, __thiscall as __fastcall --, stack arguments and return, its footprint). VP_FAITHFUL
// (career_credits.cpp has no fixes; career_shop.cpp's are on in the fix build, below).
//
// The world. Once: the $E static initialisers of libraries ui, menu, root, career and intro (their Xlators, colours,
// tables), the game's UIBegin; a locale; a CareerInfo at its own address (0x5cf078) and a season; the car's upgrade set
// (every upgrade the shop's pages list, and an entry 0) at CareerGetUpgradeSet's static; the pages' CatalogItem lists as
// the callbacks build them (and one naming an upgrade the set lacks); the upgrades' names translated by the game's
// get_upgrade_names; an UpgradeSummary (its inlined constructor's vtable) and an UpgradeCatalog (the original constructor)
// in a full-screen window by the game's WidgetCreateWindow and _UIAddItems (the catalog's Added adds its scroll bar), then
// their Creates; the credits' table by the game's fillout_credits; six credit blocks. This is the pristine state.
//
// Then for EVERY function, `rounds` times (ten times that for one that runs no dialog or loop): pristine restored; half
// the rounds POISONED (.data/.bss random but for the strings and tables the code reads -- the career's and the credits'
// among them -- and what the ui library follows); the world random: the set (0..35 entries, some named as nothing the
// pages list, sections -1..5, classes, prices, requirements -- another upgrade, none, one the set lacks), the CareerInfo
// (funds, what's bought -- 0, 1 or any byte --, class -2..6, the next event, the places), the season's prizes, the catalog
// (its rectangle, scroll, hot item -3..70, pressed, count, indexes -1..34, stamps or none), the summary's rectangle, the
// credits' table (count 0..512, types 0..6 and -1 -- pictures among them --, height), the blocks (fresh or used flags, real
// or odd sizes), the translated names' tables, every function-local Xlator of the four objects built or not (a built
// one's cookie fresh or stale) and the file-scope ones' cookies, CareerGetClassName's Xlators built or not, test mode, the
// screen grab failing once, the car file loading or not; arguments made for each function (the mouse on and off the catalog,
// prices around the funds, times across the credits' scroll); for the dialogs and loops a random input script (MouseUp: half the
// time just Enter, the yes / no box's yes), ended by a watchdog (after 40 frames the dialog's exit flag; the credits' loop sees a key). Snapshot; the stack filled with one
// pattern after the FPU is set; the ORIGINAL runs from a raw call thunk; its results kept, the snapshot restored, the
// REWRITE runs. Compared: .data/.bss/.idata and the 2 MB arena (a pointer into this thread's stack in both counts as
// equal), the return value (masked to its width), the x87 depth, bytes popped, ebx / esi / edi / ebp, faults, every stub's
// call log. Every byte the original changed must lie in the rewrite's footprint (unless replay_only); a pure one changes
// nothing. The x87 at 24 or 53 bits (alternating round pairs).
//
// Stubbed (logged; state in the arena so both passes see the same): world_root_race.cpp's set (memory, logs, input and
// time, the 2D calls, Xlator::xlate, sprintf -- a string argument it can't read printed as "?" --, files) and this
// group's callees outside its four objects: group A's career getters (CareerGetInfo returns the real 0x5cf078,
// CareerGetUpgradeSet / CareerGetSeason their statics' values), CareerIsTestMode, CareerGetCarName, CareerGetClassName,
// CareerLog, the paint kit (PaintKitDo), Xlate, LocaleMoney, LocaleFormatShortDate, TimeGetTimeOfDay, the car file
// (CarFileMakeDefaultSetup, CarFileLoad: random figures, NaN, infinities and denormals among them), Random (a watchdog:
// 20000 calls in one run raise an exception -- add_from_random with a used block spins), Win32Idle, the sound
// (SoundUnMuteCars, ResourceExists, Sound::Toss), stricmp, LogPanic. The game's own code everywhere else: the widget toolkit
// (UIDoDialog, UIDoOkBox, UIDoYesNoBox, _UIAddItems, UIStyleDraw / GetBounds / WordWrap / Height, UIAddDynamicStyle /
// UIRemoveStyle), UICustomControl's AddNotification / Dirty / AddItems, UIDialogItem's and Xlator's constructors,
// gxSetClip / gxRestoreClip, group A's CareerStatus (in the shop's dialogs), and every function of this group (each
// rewrite is checked against its original with the same callees).
//
// Built with /DVP_CAREER_FIXES, career_shop.cpp's fixes are on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Career"). Every
// function is still compared as above, with the rounds that reach a fixed case kept out of the comparison and counted
// (the rewrite still run on each and required to return cleanly, or to fault just where the original does):
// UpgradeSummary::Draw's where the original's sprintf printed an engine or weight line past its 0x50 bytes (the stub knows
// the two formats; the random car file's figures reach it: a float's 39 digits). No other fixed case is in the random
// world's reach (upgrade names of 63 characters at most, sets of 40 at most, translated names of 24 and translations of
// 29, descriptions of 272). Then directed_fix_tests: for each fix, the bad case run on the
// rewrite alone -- no fault, the bytes popped and ebx / esi / edi / ebp kept, nothing written outside what it may write
// (the memory around it compared), and the result the fix promises -- and its boundary case (the longest input that fits)
// on both, compared bit for bit. Without it (VP_FAITHFUL) every rewrite must match its original bit for bit.
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
#include <string>
#include <vector>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif

#ifndef VP_CAREER_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#define CAREER_FIXES 0
#else
#define CAREER_FIXES 1
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

#include "../hook/career_shop.cpp"
#include "../hook/career_credits.cpp"

using namespace uit;
using namespace cshop;

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
    uint32_t rs;                                  // the stubs' random answers
    int32_t test_mode;                            // CareerIsTestMode
    int32_t rand_calls;                           // Random's watchdog
    int32_t cf_load_ok;                           // CarFileLoad's answer
    int32_t time_step;                            // PTimeNow's step (ms): 16 mostly, sometimes a long frame
};
#define HS ((HState*)g_arena)
enum : uint32_t { A_HEAP = 0x100000 };
static uint32_t hs_rand() {
    uint32_t x = HS->rs;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    HS->rs = x;
    return (x * 2654435761u) ^ (x >> 16);
}
// ---- the log ----------------------------------------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 17 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void L(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static uintptr_t g_self_lo, g_self_hi;
// (64-bit sums: an address near the top with a length can't wrap into range)
static bool readable(const void* p, uint32_t n) {
    const uint64_t a = (uint64_t)(uintptr_t)p, e = a + n;
    if (a >= (uint64_t)(uintptr_t)g_arena && e <= (uint64_t)(uintptr_t)g_arena + ARENA_BYTES) return true;
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

// ---- stubs: memory, logs, sync ------------------------------------------------------------------------------------------------
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

// ---- stubs: input and time (the watchdog: after frame_limit frames the dialog's exit flag, and a key for the credits) -------
static int32_t __cdecl stub_PTimeNow() {
    L('TIME');
    HS->time += HS->time_step ? HS->time_step : 16;
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
    if (HS->frames > HS->frame_limit + 2) return 1;          // (the credits' loop: the watchdog's key)
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
static void* __cdecl stub_gxGetStamp(const char* name) {
    L('GSTP'); LS(name);
    if (readable(name, 4) && !strncmp(name, "none", 4)) return 0;          // (a stamp that isn't there)
    return fake_for(name, 'STMP');
}
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
    return readable(s, 1) ? (int32_t)strnlen(s, 0x2000) * fake_val(f, 7) : 0;
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
    if (HS->frames == HS->grab_fail_at) { HS->grab_fail_at = -1; return 0; }   // (once: the credits' loop only counts
                                                                                  // frames it grabbed)
    c->format = 4; c->flags = 0x80; c->pixels = (uint8_t*)(uintptr_t)0x7b000000u; c->w = 640; c->h = 480; c->pitch = 1280;
    c->cx0 = 0; c->cy0 = 0; c->cx1 = 640; c->cy1 = 480;
    return 1;
}
static void __cdecl stub_gxReleaseScreen() { L('RELS'); }
static void __cdecl stub_gxFlip() { L('FLIP'); }
static void __cdecl stub_mrBeginFrame() { L('MRBF'); }
static void __cdecl stub_mrEndFrame() { L('MREF'); }

// ---- stubs: text, controls, files -------------------------------------------------------------------------------------------------
// (a translation as the game's are: short, one of 16 by the key and the round, now and then a longer one; the credits'
// prize lines are formats with a number)
static const char* const k_xl_text[16] = {"Engine", "hp", "rpm", "lb-ft", "Stock", "Price", "Purchased", "Available", "Class",
                                          "Back", "Ok", "Car", "Chassis", "Wheels", "Requires the upgrade", "A longer translation of a key"};
static uint32_t g_xl_seed;
static bool key_is(uint32_t key, const char* prefix) {
    const char* k = (const char*)(uintptr_t)key;
    return readable(k, (uint32_t)strlen(prefix)) && !strncmp(k, prefix, strlen(prefix));
}
static const char* xl_text(uint32_t key) {
    if (key_is(key, "Career:Credits:")) return (hash_str((const char*)(uintptr_t)key) ^ g_xl_seed) & 1 ? "for place %d" : "prize %d, season";
    const uint32_t h = (on_stack(key) ? 0x5eedu : hash_str((const char*)(uintptr_t)key)) ^ g_xl_seed;
    return k_xl_text[(h & 0xff) < 0xf0 ? h % 14 : 14 + (h & 1)];
}
static void __fastcall stub_xlate(uint32_t* xl, int) {
    L('XLAT'); L(P(xl));
    xl[1] = (uint32_t)(uintptr_t)xl_text(xl[0]);
    xl[2] = UI_GU32(S_XLATOR_COOKIE);
}
static void __cdecl stub_LocaleConvertNumeric(char* s) { L('LCNV'); LS(s); }
static int __cdecl stub_tolower(int c) { L('TLOW'); L((uint32_t)c); return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int32_t __cdecl stub_FileOpen(const char* name) { L('FOPN'); LS(name); return 0; }
static void __cdecl stub_FileClose(int32_t* fd) { L('FCLS'); L((uint32_t)*fd); *fd = 0; }
static uint8_t __cdecl stub_ClipboardGetText(char* buf, int n) { L('CLIP'); if (n > 0) buf[0] = 0; return 0; }
// the fix build: whether the original's sprintf printed one of UpgradeSummary::Draw's lines past its 0x50 bytes
static bool g_overran;
// sprintf: the arguments logged; a %s it can't read printed as "?" (both passes print the same); the result logged
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    L('SPRF'); L(P(buf)); LS(fmt);
    va_list ap;
    va_start(ap, fmt);
    uint32_t w[24];
    memcpy(w, ap, sizeof w);
    va_end(ap);
    if (!readable(fmt, 1)) { L('BADF'); if (readable(buf, 1)) buf[0] = 0; return 0; }
    int nw = 0;
    for (const char* p = fmt; readable(p, 1) && *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.*lh", *p)) { if (*p == '*') nw++; p++; }
        if (!*p) break;
        if (*p == 's' && nw < 24) {
            LS((const char*)(uintptr_t)w[nw]);
            if (!readable((const char*)(uintptr_t)w[nw], 1)) w[nw] = (uint32_t)(uintptr_t)"?";
        }
        nw += strchr("eEfgG", *p) ? 2 : 1;
    }
    for (int i = 0; i < nw && i < 16; i++) L(on_stack(w[i]) ? 'STAK' : w[i]);
    const int r = vsprintf(buf, fmt, (va_list)w);
    if ((fmt == (const char*)0x00500710 || fmt == (const char*)0x00500704) && r > 0x4f) g_overran = true;
    LS(buf);
    return r;
}
static int __cdecl stub_stricmp(const char* a, const char* b) {
    L('SICM'); LS(a); LS(b);
    if (!readable(a, 1) || !readable(b, 1)) *(volatile int*)0 = 0;
    const int r = _stricmp(a, b);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
static void __cdecl stub_LocaleMoney(char* buf, int32_t v, uint32_t b) { L('LMNY'); L(P(buf)); L((uint32_t)v); L(b & 0xff); sprintf(buf, "$%d", v); }
static void __cdecl stub_LocaleFormatShortDate(char* buf, int32_t n, int32_t d, int32_t m, int32_t y) {
    L('DATE'); L(P(buf)); L((uint32_t)n); L((uint32_t)d); L((uint32_t)m); L((uint32_t)y);
    _snprintf(buf, (size_t)(n > 0 ? n : 0), "%d/%d/%d", d, m, y);
    if (n > 0) buf[n - 1] = 0;
}

// ---- stubs: this group's callees -----------------------------------------------------------------------------------------------
static const char* g_carname;
static const char* const k_classes[4] = {"Rookie", "Amateur", "Pro", "Legend"};
static uint8_t* __cdecl stub_CareerGetInfo() { L('CGIN'); return (uint8_t*)(uintptr_t)S_INFO; }
static void* __cdecl stub_CareerGetSeason() { L('CGSE'); return UI_GP(void, S_SEASON); }
static void* __cdecl stub_CareerGetUpgradeSet() { L('CGUS'); return UI_GP(void, S_UPGRADE_SET); }
static uint8_t __cdecl stub_CareerIsTestMode() { L('CITM'); return (uint8_t)HS->test_mode; }
static const char* __cdecl stub_CareerGetCarName() { L('CGCN'); return g_carname; }
static const char* __cdecl stub_CareerGetClassName(int32_t c) { L('CGCL'); L((uint32_t)c); return k_classes[(uint32_t)c & 3]; }
static void __cdecl stub_CareerLog(const char* fmt, ...) { L('CLOG'); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); }
static void __cdecl stub_PaintKitDo(const char* s, int32_t n) { L('PKDO'); LS(s); L((uint32_t)n); }
static const char* __cdecl stub_Xlate(const char* key) { L('XLTE'); LS(key); return xl_text((uint32_t)(uintptr_t)key); }
static void __cdecl stub_TimeGetTimeOfDay(uint32_t* t) {
    L('TOD '); L(P(t));
    for (int i = 0; i < 5; i++) t[i] = hs_rand();
}
static void __cdecl stub_CarFileMakeDefaultSetup(uint8_t* s) { L('CFDS'); L(P(s)); memset(s, 0x3c, 0xd4); }
static uint8_t __cdecl stub_CarFileLoad(uint8_t* cd, const void* setup, const char* name, const uint8_t* upg) {
    L('CFLD'); L(P(cd)); L(P(setup)); LS(name); L(P(upg));
    if (readable(upg, 0x100)) L_block(upg, 64);
    if (readable(setup, 0xd4)) L_block(setup, 0x35);
    uint32_t* w = (uint32_t*)cd;
    for (int i = 0; i < 0x1e0 / 4; i++) {
        const uint32_t r = hs_rand();
        switch (r & 15) {
        case 0: w[i] = 0x7fc00000u; break;
        case 1: w[i] = 0x7f800000u | (r & 0x80000000u); break;
        case 2: w[i] = 0x7f800001u; break;                              // signalling
        case 3: w[i] = r & 0x807fffffu; break;                          // denormal
        case 4: w[i] = 0x7f7fffffu; break;
        default: w[i] = fbits((float)(r >> 8) * 0.01f); break;
        }
    }
    return (uint8_t)HS->cf_load_ok;
}
static int32_t __cdecl stub_Random(int32_t n) {
    L('RAND'); L((uint32_t)n);
    if (++HS->rand_calls > 20000) RaiseException(0xe0000badu, 0, 0, 0);
    return n > 0 ? (int32_t)(hs_rand() % (uint32_t)n) : 0;
}

// ---- the generic stubs: logged arguments (d a dword -- a pointer into the stack as one word --, b its low byte, s a
// string), a random answer from the arena's stream (v none, b mostly 0, B mostly 1) -----------------------------------------------
#define GEN_STUBS(X)                                                                                                          \
    X(0x00412bf0, "", 'v') X(0x00471de0, "", 'v') X(0x00419d10, "s", 'b') X(0x004724c0, "sd", 'v')
struct GenStub { uint32_t at; const char* args; char ret; };
#define GS_ENTRY(a, s, r) {a, s, r},
static const GenStub g_gen[] = {GEN_STUBS(GS_ENTRY)};
enum { N_GEN = sizeof g_gen / sizeof g_gen[0] };
static uint32_t gen_body(int id, const uint32_t* a) {
    const GenStub& g = g_gen[id];
    L('GEN '); L(g.at);
    int k = 0;
    for (const char* p = g.args; *p; p++, k++) switch (*p) {
        case 'd': L(on_stack(a[k]) ? 'STAK' : a[k]); break;
        case 'b': L(a[k] & 0xff); break;
        case 's': LS((const char*)(uintptr_t)a[k]); break;
        }
    switch (g.ret) {
    case 'b': return (hs_rand() & 3) == 0 ? 1u : 0u;
    case 'B': return (hs_rand() & 7) != 0 ? 1u : 0u;
    }
    return 0;
}
template <int ID> static uint32_t __cdecl gen_stub(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    const uint32_t a[6] = {a0, a1, a2, a3, a4, a5};
    return gen_body(ID, a);
}
template <size_t... I> static void patch_gen(std::index_sequence<I...>) { (patch_jmp(g_gen[I].at, (void*)&gen_stub<(int)I>), ...); }

// ---- the raw call ---------------------------------------------------------------------------------------------------------------
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
        printf("  fault at %08x: address %08x, eax %08x ecx %08x edx %08x esi %08x edi %08x\n", (uint32_t)e->ContextRecord->Eip,
               (uint32_t)e->ExceptionRecord->ExceptionInformation[1], (uint32_t)e->ContextRecord->Eax, (uint32_t)e->ContextRecord->Ecx,
               (uint32_t)e->ContextRecord->Edx, (uint32_t)e->ContextRecord->Esi, (uint32_t)e->ContextRecord->Edi);
    return EXCEPTION_EXECUTE_HANDLER;
}
// the stack the call will use, filled with one pattern (so a local the original leaves uninitialised reads the same thing
// in both passes); called last before the raw call, after the _controlfp_s calls, top down
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
static UIDialogItem* it_(UIDialogItem* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h, const void* text,
                         int32_t i1c, const void* data, int32_t style) {
    memset(it, 0, sizeof *it);
    it->type = type; it->id = id; it->x = x; it->y = y; it->w = w; it->h = h; it->text = (const char*)text; it->i1c = i1c;
    it->data = (void*)data; it->style = style;
    return it + 1;
}
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
        if ((!strcmp(c[2], "ui") || !strcmp(c[2], "menu") || !strcmp(c[2], "root") || !strcmp(c[2], "career") ||
             !strcmp(c[2], "intro")) && c[5][0] == '$' && c[5][1] == 'E')
            g_setup_e.push_back((uint32_t)strtoul(c[0], 0, 16));
    }
    fclose(f);
}

// the pages' lists as the callbacks build them (the code's own strings), and one naming an upgrade the set lacks
static const uint32_t k_engine[] = {0x00500320, 0x00500334, 0x00500344, 0x00500354, 0x00500360, 0x00500370, 0x00500380,
                                    0x00500388, 0x00500398, 0x005003a8, 0x005003b8, 0x005003cc, 0x005003d8, 0x005003e4,
                                    0x005003f0, 0x005003fc, 0x00500408, 0x00500418, 0x00500424, 0x00500434, 0, 0};
static const uint32_t k_trans[] = {0x0050046c, 0x00500474, 0x00500484, 0x00500494, 0x005004a4, 0x005004b8, 0x005004c4,
                                   0x005004d4, 0, 0};
static const uint32_t k_susp[] = {0x00500500, 0x00500510, 0x00500520, 0x00500534, 0x00500544, 0x00500550, 0x0050055c,
                                  0x0050056c, 0x00500578, 0x00500588, 0x00500598, 0x005005a8, 0x005005b4, 0x005005c4, 0, 0};
static const uint32_t k_wheels[] = {0x005005ec, 0x005005f8, 0x00500604, 0x00500614, 0x00500620, 0x0050062c, 0x00500638,
                                    0x00500648, 0, 0};
static const uint32_t k_body[] = {0x0050066c, 0, 0x0050067c, 0, 0x0050068c, 0, 0x005006a0, 0, 0x005006b4, 0, 0x005006c8, 0, 0, 0};
enum { N_LISTS = 6, N_UPG = 40 };
struct World_ {
    gxCanvas* screen;
    gxCanvas* canvas2;
    WidgetWindow* win;
    uint8_t* locale;
    CatalogItem* lists[N_LISTS];
    CshUpgradeSet* set;                           // 4 + 4 * N_UPG
    CshUpgrade* upg;                              // N_UPG of 0x6c
    char* upg_names[N_UPG];                       // each one's real name
    int nupg;
    uint8_t* season;                              // 0x24 + 0x58 * 40
    UICustomControl* summary;
    UpgradeCatalog* catalog;
    UpgradeCatalog* scratch_cat;
    uint8_t* scratch;
    int32_t* py;
    RandomCreditTableItem* rct;                   // 8
    CreditItem* blocks;                           // arena blocks (0x40 items)
    CreditItem* items;                            // loose items (8)
    char* texts[16];
    char* strs[48];                               // translated names / descriptions
    uint32_t credits_n, credits_h;                // as fillout_credits left them
    std::vector<Widget*> widgets;
};
static World_ W;

static void build_world() {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    HS->rs = 0x12345;
    HS->cf_load_ok = 1;
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
    W.locale = (uint8_t*)wv(0x40);
    UI_GP(uint8_t, 0x00509354) = W.locale;
    *(float*)(W.locale + 0x14) = 1.0f; *(float*)(W.locale + 0x1c) = 1.0f;
    *(const char**)(W.locale + 0x30) = wstr("lb");
    *(float*)(W.locale + 0x34) = 1.0f;
    g_carname = wstr("viper");
    for (int i = 0; i < 16; i++) W.texts[i] = (char*)wv(0x60);
    for (int i = 0; i < 48; i++) W.strs[i] = (char*)wv(0x120);
    // the set: entry 0, then every upgrade the pages list, then three others
    W.set = (CshUpgradeSet*)wv(4 + 4 * N_UPG);
    W.upg = (CshUpgrade*)wv(0x6c * N_UPG);
    int n = 0;
    auto add = [&](const char* nm) {
        CshUpgrade* u = (CshUpgrade*)((uint8_t*)W.upg + 0x6c * n);
        strncpy(u->name, nm, 0x3f);
        W.upg_names[n] = wstr(nm);
        W.set->items[n] = u;
        n++;
    };
    add("Stock");
    const uint32_t* lists[5] = {k_engine, k_trans, k_susp, k_wheels, k_body};
    const int lens[5] = {10, 4, 7, 4, 6};
    for (int l = 0; l < 5; l++)
        for (int k = 0; k < lens[l]; k++) add((const char*)(uintptr_t)lists[l][2 * k]);
    add("NitrousKit"); add("SpareTire"); add("RacingSeats");
    W.nupg = n;
    W.set->count = n;
    UI_GP(CshUpgradeSet, S_UPGRADE_SET) = W.set;
    for (int l = 0; l < 5; l++) {
        W.lists[l] = (CatalogItem*)wv(8 * 12);
        memcpy(W.lists[l], lists[l], 8 * (lens[l] + 1));
    }
    W.lists[5] = (CatalogItem*)wv(8 * 12);
    memcpy(W.lists[5], k_trans, 8 * 5);
    W.lists[5][2].name = wstr("NoSuchUpgrade");
    W.lists[5][3].stamp = wstr("none.stp");
    W.season = (uint8_t*)wv(0x24 + 0x58 * 40);
    UI_GP(uint8_t, S_SEASON) = W.season;
    strcpy((char*)(uintptr_t)S_INFO, "Tester");
    call_orig(F_get_upgrade_names, 0, {});
    // the summary (its inlined constructor), the catalog (the original constructor), in a full-screen window
    W.summary = (UICustomControl*)wv(0x18);
    W.summary->vtbl = (const void*)(uintptr_t)VT_UpgradeSummary;
    W.catalog = (UpgradeCatalog*)wv(sizeof(UpgradeCatalog));
    call_orig(F_UpgradeCatalog_ctor, 1, {U(W.catalog), 0, U(W.lists[0])});
    W.scratch_cat = (UpgradeCatalog*)wv(sizeof(UpgradeCatalog));
    W.scratch = (uint8_t*)wv(0x400);
    W.py = (int32_t*)wv(16);
    UIDialogItem* items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 4);
    UIDialogItem* it = items;
    it = it_(it, 23, 0, 0x28, 0x5f, 0x1ae, 0x10e, "", 0, W.summary, 0);
    it = it_(it, 23, 0, 0x2d, 0x5a, 0x217, 0x11d, "", 0, W.catalog, 0);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    call_orig(F_WidgetCreateWindow, 0, {0, 0, 0, 640, 480, 2});
    W.win = (WidgetWindow*)(uintptr_t)g_setup_ret;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    call_orig(F_UIAddItems, 0, {U(W.win), U(items)});
    const void* ctl[] = {W.summary, W.catalog};
    for (const void* c : ctl) call_orig((*(const uint32_t* const*)c)[1], 1, {U(c), 0});
    for (int i = 0; i < W.win->count; i++) W.widgets.push_back((Widget*)W.win->widgets[i]);
    // the credits
    call_orig(F_fillout_credits, 0, {0x12});
    W.credits_n = UI_GU32(S_CREDITS_N);
    W.credits_h = UI_GU32(S_CREDITS_H);
    W.rct = (RandomCreditTableItem*)wv(8 * sizeof(RandomCreditTableItem));
    W.blocks = (CreditItem*)wv(0x40 * sizeof(CreditItem));
    W.items = (CreditItem*)wv(8 * sizeof(CreditItem));
    if (getenv("VP_DEBUG_WORLD"))
        printf("  world: %d widgets; %d upgrades; credits %u items, %u high; catalog count %d\n", W.win->count, W.set->count,
               W.credits_n, W.credits_h, W.catalog->count);
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
// every function-local Xlator of this group: {guard, bit, Xlator, key}
static const uint32_t k_xl[][4] = {
    {0x005d3768, 0x01, 0x005d3d08, 0x00500238}, {0x005d3768, 0x02, 0x005d3c80, 0x0050024c}, {0x005d3768, 0x04, 0x005d3d28, 0x0050025c},
    {0x005d3768, 0x08, 0x005d3cc0, 0x0050026c}, {0x005d3768, 0x10, 0x005d4188, 0x0050027c}, {0x005d3768, 0x20, 0x005d3848, 0x00500290},
    {0x005d3768, 0x40, 0x005d3770, 0x005002a0}, {0x005d3768, 0x80, 0x005d3cf8, 0x005002b0},
    {0x005d3cec, 0x01, 0x005d3810, 0x005002c4}, {0x005d3cec, 0x02, 0x005d3c98, 0x005002d4}, {0x005d3cec, 0x04, 0x005d3ce0, 0x005002e4},
    {0x005d3ca8, 0x01, 0x005d3758, 0x00500440}, {0x005d4180, 0x01, 0x005d37e0, 0x005004e0}, {0x005d3834, 0x01, 0x005d3838, 0x005005d0},
    {0x005d3754, 0x01, 0x005d3858, 0x00500654}, {0x005d3ca4, 0x01, 0x005d3d70, 0x005006dc}, {0x005d3d50, 0x01, 0x005d3d18, 0x00500458},
    {0x005d5cfc, 0x01, 0x005d5d00, 0x005006f4},
    {0x005d5cdc, 0x01, 0x005d5cc0, 0x005007b4}, {0x005d5cdc, 0x02, 0x005d5cd0, 0x00500788}, {0x005d5cdc, 0x04, 0x005d5ce0, 0x0050076c},
    {0x005d5cdc, 0x08, 0x005d5cf0, 0x00500758},
    {0x005d5cac, 0x01, 0x005d5c70, 0x0050082c}, {0x005d5cac, 0x02, 0x005d5c90, 0x00500818}, {0x005d5cac, 0x04, 0x005d5ca0, 0x00500804},
    {0x005d5cac, 0x08, 0x005d5c80, 0x005007f4}, {0x005d5cac, 0x10, 0x005d5cb0, 0x005007c8},
    {0x005d4204, 0x01, 0x005d4248, 0x00500900}, {0x005d4204, 0x02, 0x005d4238, 0x0050091c}, {0x005d4204, 0x04, 0x005d4220, 0x00500938},
};
// the file-scope ones ($E): {Xlator, key}
static const uint32_t k_xl_file[][2] = {
    {0x005d3780, 0x005000d8}, {0x005d3800, 0x005000f8}, {0x005d3828, 0x00500114}, {0x005d3cb0, 0x00500134}, {0x005d3d38, 0x00500154},
    {0x005d3d60, 0x00500178}, {0x005d3c70, 0x00500198}, {0x005d3748, 0x005001b4}, {0x005d37f0, 0x005001d4}, {0x005d3cd0, 0x005001f4},
    {0x005d3790, 0x0050021c},
};
static const char k_chars[] = "abcdefghij klmnopqrstuvwxyz ABCDEFGH 0123456789 .,!-";
static void random_text(char* s, int maxlen) {
    const int n = irange(0, maxlen);
    for (int i = 0; i < n; i++) s[i] = k_chars[rnd() % (sizeof k_chars - 1)];
    s[n] = 0;
}
static void set_xl(uint32_t xl, uint32_t key, bool fresh) {
    UI_GU32(xl) = key;
    UI_GU32(xl + 4) = U(xl_text(key));
    UI_GU32(xl + 8) = fresh ? UI_GU32(S_XLATOR_COOKIE) : ~UI_GU32(S_XLATOR_COOKIE);
}
static void random_credit(CreditItem* c) {
    const uint32_t r = rnd() % 100;
    c->type = r < 30 ? 0 : r < 55 ? 3 : r < 68 ? 1 : r < 76 ? 2 : r < 90 ? 5 : r < 95 ? 4 : r < 98 ? 6 : -1;
    char* t = W.texts[rnd() % 16];
    c->text = c->type == 4 ? (chance(20) ? "none.stp" : "logo.stp") : t;
}
static void randomize_world() {
    // the set
    CshUpgradeSet* s = W.set;
    s->count = chance(85) ? W.nupg - irange(0, 3) : irange(0, W.nupg);
    for (int i = 0; i < W.nupg; i++) {
        CshUpgrade* u = (CshUpgrade*)((uint8_t*)W.upg + 0x6c * i);
        if (chance(4)) random_text(u->name, 20);
        else strcpy(u->name, W.upg_names[i]);
        u->section = chance(92) ? irange(0, 2) : irange(-1, 5);
        u->req_class = chance(85) ? irange(0, 4) : irange(-2, 7);
        u->price = chance(85) ? irange(0, 60000) : (int32_t)rnd();
        if (chance(65)) u->requires[0] = 0;
        else if (chance(85)) strcpy(u->requires, W.upg_names[irange(0, W.nupg - 1)]);
        else strcpy(u->requires, "NotInTheSet");
    }
    // the CareerInfo (its own address) and the season
    uint8_t* info = (uint8_t*)(uintptr_t)S_INFO;
    random_text((char*)info, 14);
    *(int32_t*)(info + 0x18) = chance(80) ? irange(0, 80000) : (int32_t)rnd();
    const int bought = irange(0, 100);
    for (int i = 0; i < 0x100; i++) info[0x1c + i] = (uint8_t)(chance(95) ? chance(bought) : rnd());
    *(int32_t*)(info + 0x124) = chance(90) ? irange(0, 3) : irange(-2, 6);
    *(int32_t*)(info + 0x128) = chance(90) ? irange(1, 12) : irange(-2, 40);
    for (int i = 0; i < 40; i++) *(int32_t*)(info + 0x530 + 4 * i) = chance(90) ? irange(0, 8) : irange(-3, 12);
    for (int i = 0; i < 8; i++) *(int32_t*)(W.season + 4 + 4 * i) = chance(20) ? 0 : irange(-100, 100000);
    for (int e = 0; e < 40; e++)
        for (int p = 0; p < 8; p++) *(int32_t*)(W.season + 0x24 + 0x58 * e + 0x18 + 4 * p) = chance(15) ? 0 : irange(-50, 50000);
    // the summary and the catalog
    UICustomControl* sm = W.summary;
    if (chance(20)) { sm->x = irange(-50, 600); sm->y = irange(-50, 460); }
    UpgradeCatalog* c = W.catalog;
    if (chance(25)) { c->x = irange(-50, 600); c->y = irange(-50, 460); c->w = irange(-10, 600); c->h = irange(-10, 500); }
    c->axis.pos = chance(80) ? irange(0, 8) : irange(-5, 40);
    c->hot = chance(85) ? irange(-1, 10) : irange(-3, 70);
    c->pressed = (uint8_t)(chance(90) ? rnd() & 1 : rnd());
    if (chance(20)) c->count = irange(0, 12);
    for (int i = 0; i < 64; i++) {
        if (chance(85)) c->index[i] = irange(0, s->count > 0 ? s->count - 1 : 0);
        else c->index[i] = irange(-1, 34);
        if (chance(15)) c->stamps[i] = chance(50) ? 0 : fake_for(W.texts[i & 15], 'STMP');
    }
    for (Widget* g : W.widgets) { g->visible = chance(90); g->enabled = chance(90); g->dirty = chance(50); }
    // the credits: the table as fillout_credits left it, or random; the blocks, loose items
    CreditItem* t = (CreditItem*)(uintptr_t)S_CREDITS;
    if (chance(60)) {
        UI_GU32(S_CREDITS_N) = chance(80) ? W.credits_n : (uint32_t)irange(0, 0x200);
        UI_GU32(S_CREDITS_H) = chance(80) ? W.credits_h : (uint32_t)irange(-100, 20000);
        for (int i = 0; i < 0x200; i++)
            if (chance(3)) random_credit(&t[i]);
    } else {
        UI_GU32(S_CREDITS_N) = chance(90) ? (uint32_t)irange(0, 0x200) : (uint32_t)irange(-5, 0x220);
        UI_GU32(S_CREDITS_H) = chance(90) ? (uint32_t)irange(0, 20000) : rnd();
        const bool pics = chance(30);
        for (int i = 0; i < 0x200; i++) {
            random_credit(&t[i]);
            if (!pics && t[i].type == 4) t[i].type = 3;
        }
    }
    for (int i = 0; i < 0x40; i++) {
        random_credit(&W.blocks[i]);
        if (W.blocks[i].type == 4 && chance(80)) W.blocks[i].type = 0;
    }
    for (int i = 0; i < 8; i++) random_credit(&W.items[i]);
    W.items[0].type = 4; W.items[0].text = "logo.stp";                  // (two pictures among the loose items, one missing)
    W.items[1].type = 4; W.items[1].text = "none.stp";
    static const uint32_t real[6][2] = {{0x005010e8, 0x30}, {0x00501118, 0x38}, {0x00501150, 0x38},
                                        {0x005011b8, 0x30}, {0x005011e8, 0x38}, {0x00501188, 0x30}};
    for (int i = 0; i < 8; i++) {
        RandomCreditTableItem* r = &W.rct[i];
        if (chance(60)) { r->items = (const CreditItem*)(uintptr_t)real[i % 6][0]; r->bytes = real[i % 6][1]; }
        else { r->items = W.blocks + irange(0, 0x30); r->bytes = chance(80) ? 8u * (uint32_t)irange(0, 10) : (uint32_t)irange(0, 0x50); }
        r->used = (uint8_t)(chance(96) ? 0 : chance(50) ? 1 : rnd());
        r->_9[0] = (uint8_t)rnd();
    }
    for (int i = 0; i < 16; i++) random_text(W.texts[i], i < 12 ? 20 : 80);
    for (int i = 0; i < 48; i++) random_text(W.strs[i], i < 40 ? 24 : 0x110);
    // the stubs' world
    HS->time = (int32_t)rnd();
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->grab_fail_at = chance(20) ? irange(1, 6) : -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->script_n = HS->script_pos = 0;
    HS->rs = rnd() | 1;
    HS->test_mode = chance(50);
    HS->rand_calls = 0;
    HS->cf_load_ok = chance(80);
    HS->time_step = chance(80) ? 16 : irange(100, 900);
    g_xl_seed = rnd() & 3;
}
// the statics, after poisoning too
static void randomize_statics() {
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_GU32(S_DIALOG_IDLE) = 0;
    UI_G8(S_EXIT) = 0;
    UI_GF(S_UI_DT) = chance(10) ? wildf() : range(0.0f, 0.25f);
    UI_GP(void, S_CURSOR) = 0;
    UI_G32(S_MOUSE_X) = irange(0, 639); UI_G32(S_MOUSE_Y) = irange(0, 479);
    UI_GP(uint8_t, 0x00509354) = W.locale;
    *(float*)(W.locale + 0x34) = chance(50) ? 1.0f : chance(80) ? range(0.0f, 3.0f) : wildf();
    UI_GP(CshUpgradeSet, S_UPGRADE_SET) = W.set;
    UI_GP(uint8_t, S_SEASON) = W.season;
    for (int i = 0; i < 256; i++) {
        UI_GP(char, S_UP_NAMES + 4u * i) = W.strs[rnd() % 40];
        UI_GP(char, S_UP_DESCS + 4u * i) = W.strs[chance(80) ? rnd() % 40 : 40 + rnd() % 8];
    }
    const bool all_built = chance(50);
    for (auto& x : k_xl) {                              // each function-local Xlator built (fresh or stale) or not
        if (all_built || chance(75)) {
            UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) | x[1]);
            set_xl(x[2], x[3], chance(70));
        } else {
            UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) & ~x[1]);
        }
    }
    for (auto& x : k_xl_file) set_xl(x[0], x[1], chance(70));
    UI_G8(S_CLASS_XL_GUARD) = (uint8_t)(all_built || chance(70) ? 0x0f : rnd() & 0x0f);
}
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x24, 0x31, 0x32, 0x121, 0x122, 0x124, 0x170, 0x171, 'y', 'n', 'Y'};
    const int n = irange(0, maxops);
    for (int i = 0; i < n && i < 62; i++) {
        int32_t* op = HS->script[i];
        const uint32_t r = rnd() % 12;
        op[1] = op[2] = 0;
        if (r < 4) { op[0] = OP_MOVE_W; op[1] = irange(0, 40); op[2] = irange(0, 479); }
        else if (r < 5) { op[0] = OP_MOVE_XY; op[1] = irange(-10, 650); op[2] = irange(-10, 490); }
        else if (r < 6) op[0] = OP_DOWN;
        else if (r < 7) op[0] = OP_UP;
        else if (r < 8) op[0] = OP_RDOWN;
        else if (r < 11) { op[0] = OP_KEY; op[1] = keys[rnd() % (sizeof keys / sizeof keys[0])]; }
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
        {0x004e4db8, 0x004e4dbc},                                       // ""
        {0x004e5778, 0x004e5a20},                                       // root's strings ("%s")
        {0x004f665c, 0x004f6698}, {0x004f6b78, 0x004f7228}, {0x004f7230, 0x004f7300},   // the ui library's keys and strings
        {0x004efbf8, 0x004efbfc},
        {0x004ff5a8, 0x005024b0},                                       // the career's and the credits' strings and tables
        {0x005024ac, 0x005024c8},                                       // the CRT's FPU flags
        {0x00502630, 0x00502680}, {0x005026f8, 0x00502730}, {0x00502780, 0x005028c8},   // its jump tables and constants
        {0x005228d4, 0x005228d8}, {0x005228f4, 0x005228f8},
        {0x00578cd0, 0x00578cdc}, {0x00578d50, 0x00578d5c}, {0x00578d60, 0x00578d6c}, {0x00578e88, 0x00578e94},
        {0x00578e98, 0x00578ea4}, {0x00578eb0, 0x00578ebc}, {0x00578ec0, 0x00578ecc}, {0x00578ef8, 0x00578f04},
        {0x00579050, 0x0057905c},
        {0x00578d08, 0x00578d40},                                       // the end item
        {0x00578d48, 0x00578d4c}, {0x00578e78, 0x00578e7c}, {0x00578f40, 0x00578f44}, {0x00578e80, 0x00578e84},
        {0x005790e0, 0x005796e0},                                       // the styles (UIAddDynamicStyle's slots)
        {0x00579730, 0x00579778},                                       // the running window, the window table
        {0x005d45d0, 0x005d55d8},                                       // the credits' table (randomized on its own)
    };
    for (auto& k : keep) memcpy(DATA + (k[0] - 0x004e1000), keepfrom.data + (k[0] - 0x004e1000), k[1] - k[0]);
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static uint32_t canvas_arg() { return chance(70) ? U(W.screen) : U(W.canvas2); }
static void catalog_xy(uint32_t* a) {
    const UpgradeCatalog* c = W.catalog;
    if (chance(85)) {
        a[0] = (uint32_t)irange(c->x - 20, c->x + (c->w > 0 ? c->w : 0) + 20);
        a[1] = (uint32_t)irange(c->y - 60, c->y + (c->h > 0 ? c->h : 0) + 60);
    } else {
        a[0] = (uint32_t)irange(-1000, 2000);
        a[1] = (uint32_t)irange(-3000, 3000);
    }
}
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const char* nm = f.name;
    uint32_t* a = f.fast ? w + 2 : w;
    memset(W.scratch, 0xa5, 0x400);
#define IS(s) (!strcmp(nm, s))
#define HAS(s) (strstr(nm, s) != 0)
    if (HAS("deleting destructor")) a[0] = rnd() & 3;
    if (IS("upgrade_page")) { a[0] = U(W.texts[rnd() % 16]); a[1] = U(W.lists[rnd() % N_LISTS]); }
    else if (IS("gxHollowRect")) { for (int i = 0; i < 4; i++) a[i] = (uint32_t)irange(-100, 700); }
    else if (IS("can_afford") || IS("debit_account") || IS("money_string")) {
        const int32_t funds = *(int32_t*)(uintptr_t)S_FUNDS;
        a[0] = chance(70) ? (uint32_t)(funds + irange(-3, 3)) : chance(50) ? (uint32_t)irange(-1000, 100000) : rnd();
    }
    else if (HAS("UpgradeSummary::") && !HAS("XL_")) {
        w[0] = U(W.summary);
        if (HAS("scalar deleting")) { memcpy(W.scratch, W.summary, 0x18); w[0] = U(W.scratch); }
        if (HAS("::Draw")) a[0] = canvas_arg();
        if (HAS("draw_class")) {
            a[0] = chance(85) ? (uint32_t)irange(0, 3) : (uint32_t)irange(-2, 6);
            a[1] = (uint32_t)irange(-10, 400); a[2] = (uint32_t)irange(-10, 600);
            *W.py = irange(-50, 480);
            a[3] = U(W.py);
        }
        if (HAS("Callback")) { a[0] = rnd() & 3; a[1] = (uint32_t)(S_UPGRADES); }
    }
    else if (IS("UpgradeCatalog::UpgradeCatalog")) {
        w[0] = U(W.scratch_cat);
        memset(W.scratch_cat, 0xa5, sizeof(UpgradeCatalog));
        a[0] = U(W.lists[rnd() % N_LISTS]);
    }
    else if (HAS("UpgradeCatalog::") && !HAS("XL_")) {
        w[0] = U(W.catalog);
        if (HAS("scalar deleting")) { memcpy(W.scratch_cat, W.catalog, sizeof(UpgradeCatalog)); w[0] = U(W.scratch_cat); }
        if (HAS("::Draw")) a[0] = canvas_arg();
        if (HAS("Mouse")) catalog_xy(a);
        if (HAS("MouseUp") && chance(50)) {                              // (Enter: the yes / no box's yes, or Ok)
            HS->script_n = 1; HS->script_pos = 0; HS->script[0][0] = OP_KEY; HS->script[0][1] = 0xd;
        }
        if (HAS("Callback")) { a[0] = rnd() & 3; a[1] = U(&W.catalog->hot); }
    }
    else if (IS("CreditsDo(int)")) a[0] = chance(50) ? (uint32_t)-1 : (uint32_t)irange(-3, 8);
    else if (IS("fillout_credits")) a[0] = chance(80) ? 0x12u : (uint32_t)irange(0, 31);
    else if (IS("add_from_random")) {
        a[0] = U(W.rct);
        a[1] = chance(80) ? 6u : (uint32_t)irange(-2, 8);
    }
    else if (IS("item_height")) {
        a[0] = chance(60) ? U((CreditItem*)(uintptr_t)S_CREDITS + irange(0, 0x1ff)) : U(&W.items[rnd() % 8]);
        a[1] = chance(80) ? 0x12u : (uint32_t)irange(0, 31);
    }
    else if (IS("draw_credits")) {
        a[0] = canvas_arg();
        a[1] = chance(80) ? (uint32_t)irange(-2000, 120000) : rnd();
        a[2] = chance(80) ? 0x12u : (uint32_t)irange(0, 31);
    }
    else if (IS("draw_item")) {
        a[0] = (uint32_t)irange(-100, 700); a[1] = (uint32_t)irange(-100, 600);
        a[2] = chance(60) ? U((CreditItem*)(uintptr_t)S_CREDITS + irange(0, 0x1ff)) : U(&W.items[rnd() % 8]);
        a[3] = chance(80) ? 0x12u : (uint32_t)irange(0, 31);
    }
    else if (IS("IntroPlayVideo")) a[0] = U(W.texts[0]);
#undef IS
#undef HAS
    return true;
}
// VP_COVER=1: per function, in how many rounds the original's log held each of these calls (the paths reached)
static const uint32_t k_cover[] = {'CLOG', 'PKDO', 'PANC', 'CFLD', 'RAND', 'WDOG', 'CGCL', 'GSTP', 'FSTP', 'DEL ', 'LMNY', 'ATEX',
                                   'DATE', 'CITM', 'KHIT', 'GRAB', 'SICM', 'SPRF'};
enum { N_COVER = sizeof k_cover / sizeof k_cover[0] };
static bool is_modal(const char* nm) {
    static const char* const m[] = {"UpgradeDo", "upgrade_page", "engine_cb", "transmission_cb", "suspension_cb", "wheels_cb",
                                    "body_cb", "CreditsDo(int)", "CreditsDo(void)"};
    for (const char* s : m) if (!strcmp(nm, s)) return true;
    return false;
}

#if CAREER_FIXES
// ---- the fixes, each on its bad case (directed_fix_tests) -----------------------------------------------------------------
// The rewrite alone (the original would overrun there -- this program's stack or the game's statics among what it would
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
// a callee replaced for a test (a jump to a recorder), put back after
struct FxPatch {
    uint32_t at;
    uint8_t b[5];
    FxPatch(uint32_t a, void* to) : at(a) { memcpy(b, (const void*)(uintptr_t)a, 5); patch_jmp(a, to); }
    ~FxPatch() { memcpy((void*)(uintptr_t)at, b, 5); }
};
// the recorders: Xlate's keys (each answered "xlated"), UIDoOkBox's title and message
static std::vector<std::string> g_fx_keys;
static const char* __cdecl fx_Xlate(const char* key) {
    L('XLTE'); LS(key);
    g_fx_keys.push_back(readable(key, 1) ? std::string(key, strnlen(key, 0x1000)) : std::string("?"));
    return "xlated";
}
static uint32_t g_fx_dlg_off;                               // (the dialog's caller's frame: 0x200 bytes from the UIDialog + this)
static uint8_t g_fx_dlg[0x200];
static int32_t __cdecl fx_UIDoDialog(const void* d, int32_t, int32_t, int32_t, int32_t, int32_t) {
    L('DLG '); L(P(d));
    if (g_fx_dlg_off && readable((const uint8_t*)d + g_fx_dlg_off, 0x200)) memcpy(g_fx_dlg, (const uint8_t*)d + g_fx_dlg_off, 0x200);
    return -1;
}
// UIStyleDraw: each text drawn recorded, with its style
static std::vector<std::string> g_fx_draws;
static std::vector<int32_t> g_fx_draw_style;
static void __cdecl fx_UIStyleDraw(int32_t style, int32_t x, int32_t y, const char* s, uint32_t fl) {
    L('SDRW'); L((uint32_t)style); L((uint32_t)x); L((uint32_t)y); LS(s); L(fl);
    g_fx_draws.push_back(readable(s, 1) ? std::string(s, strnlen(s, 0x2000)) : std::string("?"));
    g_fx_draw_style.push_back(style);
}
// CarFileLoad: the car's data zeroed but for the figures the summary shows (power, its rpm, torque, its rpm, weight)
static float g_fx_cd[5];
static uint8_t __cdecl fx_CarFileLoad(uint8_t* cd, const void*, const char* name, const uint8_t*) {
    L('CFLD'); LS(name);
    memset(cd, 0, 0x1e0);
    memcpy(cd + 0x6c, &g_fx_cd[0], 4); memcpy(cd + 0x74, &g_fx_cd[1], 4); memcpy(cd + 0x68, &g_fx_cd[2], 4);
    memcpy(cd + 0x70, &g_fx_cd[3], 4); memcpy(cd + 0x8, &g_fx_cd[4], 4);
    return 1;
}
static std::string g_fx_box_title, g_fx_box_msg;
static int g_fx_box_n;
static void __cdecl fx_UIDoOkBox(const char* t, const char* msg) {
    L('OKBX'); LS(t); LS(msg);
    g_fx_box_title = readable(t, 1) ? std::string(t, strnlen(t, 0x1000)) : std::string("?");
    g_fx_box_msg = readable(msg, 1) ? std::string(msg, strnlen(msg, 0x1000)) : std::string("?");
    g_fx_box_n++;
}
// the tests' world: pristine, every function-local and file-scope Xlator built (fresh), the statics the code follows
static void fx_reset() {
    mem_load(g_pristine);
    g_xl_seed = 0;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_GU32(S_DIALOG_IDLE) = 0;
    UI_G8(S_EXIT) = 0;
    UI_GF(S_UI_DT) = 0.05f;
    UI_GP(void, S_CURSOR) = 0;
    UI_G32(S_MOUSE_X) = 320; UI_G32(S_MOUSE_Y) = 240;
    UI_GP(uint8_t, 0x00509354) = W.locale;
    UI_GP(CshUpgradeSet, S_UPGRADE_SET) = W.set;
    UI_GP(uint8_t, S_SEASON) = W.season;
    for (auto& x : k_xl) {
        UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) | x[1]);
        set_xl(x[2], x[3], true);
    }
    for (auto& x : k_xl_file) set_xl(x[0], x[1], true);
    UI_G8(S_CLASS_XL_GUARD) = 0x0f;
    HS->time = 1000; HS->frames = 0; HS->frame_limit = 40; HS->grab_fail_at = -1;
    HS->mouse_x = 320; HS->mouse_y = 240;
    HS->script_n = HS->script_pos = 0;
    HS->rs = 0x1234567;
    HS->test_mode = 0; HS->rand_calls = 0; HS->cf_load_ok = 1; HS->time_step = 16;
    g_fx_keys.clear();
    g_fx_box_n = 0;
    g_fx_dlg_off = 0;
    g_fx_draws.clear();
    g_fx_draw_style.clear();
}
static char g_fx_s[8][0x2000];
// a string of n c's and then tail (in one of eight buffers of this program's)
static const char* mk(int k, char c, int n, const char* tail = "") {
    memset(g_fx_s[k], c, (size_t)n);
    strcpy(g_fx_s[k] + n, tail);
    return g_fx_s[k];
}
// s's first n characters
static std::string first(const char* s, size_t n) { return std::string(s, strnlen(s, n)); }

static int directed_fix_tests() {
    Result r;
    uint32_t o;
    char m[512];

    // ---- 8. get_upgrade_names: a long upgrade name; a set of more than 256 ----
    {
        const Ent& f = fx_fn("get_upgrade_names");
        FxPatch xl(F_Xlate, (void*)&fx_Xlate);
        // (the set's entries are read for their names alone: a name of this program's stands in for a CarUpgrade)
        static uint32_t set3[4];
        for (int n : {242, 600}) {
            fx_reset();
            const char* nm = mk(0, 'u', n);
            set3[0] = 3; set3[1] = U(W.set->items[0]); set3[2] = U(nm); set3[3] = U(W.set->items[1]);
            UI_GU32(S_UPGRADE_SET) = U(set3);
            mem_save(g_snap);
            r = fx_run(f, true, {});
            sprintf(m, "get_upgrade_names, a %d-character upgrade name: a clean return", n);
            fx_check(fx_clean(f, r), m, &r);
            const std::string cut = first(nm, 241);
            sprintf(m, "get_upgrade_names, a %d-character upgrade name: its keys hold its first 241 characters (255 in all)", n);
            fx_check(g_fx_keys.size() == 6 && g_fx_keys[2] == "Upgrades:" + cut + ":Name" && g_fx_keys[3] == "Upgrades:" + cut + ":Desc" &&
                         g_fx_keys[2].size() == 255 && g_fx_keys[0] == "Upgrades:Stock:Name", m);
            bool tab = true;
            for (uint32_t k = 0; k < 3; k++)
                tab &= !strcmp(UI_GP(const char, S_UP_NAMES + 4u * k), "xlated") && !strcmp(UI_GP(const char, S_UP_DESCS + 4u * k), "xlated");
            sprintf(m, "get_upgrade_names, a %d-character upgrade name: the three entries translated", n);
            fx_check(tab, m);
            o = fx_outside(g_snap, {{(void*)(uintptr_t)S_UP_NAMES, 12}, {(void*)(uintptr_t)S_UP_DESCS, 12}});
            sprintf(m, "get_upgrade_names, a %d-character upgrade name: nothing written outside the tables' three entries", n);
            fx_check(o == 0, m, 0, o);
        }
        // a set of 300: the first 256 translated, nothing past the tables (their colours and Xlators after them)
        static uint32_t big[1 + 300];
        big[0] = 300;
        for (int i = 0; i < 300; i++) big[1 + i] = U(W.set->items[i % W.nupg]);
        fx_reset();
        UI_GU32(S_UPGRADE_SET) = U(big);
        for (uint32_t k = 0; k < 256; k++) { UI_GU32(S_UP_NAMES + 4u * k) = 0; UI_GU32(S_UP_DESCS + 4u * k) = 0; }
        mem_save(g_snap);
        r = fx_run(f, true, {});
        fx_check(fx_clean(f, r), "get_upgrade_names, a set of 300: a clean return", &r);
        bool all = g_fx_keys.size() == 512;
        for (uint32_t k = 0; all && k < 256; k++) all = UI_GU32(S_UP_NAMES + 4u * k) != 0 && UI_GU32(S_UP_DESCS + 4u * k) != 0;
        sprintf(m, "get_upgrade_names, a set of 300: the first 256 translated (%u keys)", (unsigned)g_fx_keys.size());
        fx_check(all, m);
        o = fx_outside(g_snap, {{(void*)(uintptr_t)S_UP_NAMES, 0x400}, {(void*)(uintptr_t)S_UP_DESCS, 0x400}});
        fx_check(o == 0, "get_upgrade_names, a set of 300: nothing written outside the two tables", 0, o);
        // the boundaries: a 241-character name (keys of 255); a set of 256 (the tables full)
        fx_reset();
        set3[0] = 3; set3[1] = U(W.set->items[0]); set3[2] = U(mk(0, 'u', 241)); set3[3] = U(W.set->items[1]);
        UI_GU32(S_UPGRADE_SET) = U(set3);
        fx_same(f, {}, "get_upgrade_names, a 241-character upgrade name (keys of 255)");
        fx_reset();
        big[0] = 256;
        UI_GU32(S_UPGRADE_SET) = U(big);
        fx_same(f, {}, "get_upgrade_names, a set of 256");
    }

    // ---- 9. UpgradeCatalog::MouseUp: a long "requires" message ----
    {
        const Ent& f = fx_fn("UpgradeCatalog::MouseUp");
        FxPatch ok(F_UIDoOkBox, (void*)&fx_UIDoOkBox);
        UpgradeCatalog* c = W.catalog;
        // the click: the catalog's item 0, upgrade 1, allowed in the class, requiring upgrade 2, neither bought
        auto setup = [&](const char* req_name, const char* msg) {
            fx_reset();
            c->x = 0; c->y = 0; c->w = 500; c->h = 400; c->axis.pos = 0; c->count = 1; c->index[0] = 1; c->pressed = 1; c->hot = -1;
            CshUpgrade* u1 = W.set->items[1];
            u1->req_class = 0;
            strcpy(u1->requires, W.upg_names[2]);
            W.set->count = W.nupg;
            uint8_t* info = (uint8_t*)(uintptr_t)S_INFO;
            info[0x1c + 1] = 0;
            info[0x1c + 2] = 0;
            *(int32_t*)(info + 0x124) = 1;
            UI_GP(const char, S_UP_NAMES + 4) = "the upgrade";
            UI_GP(const char, S_UP_NAMES + 8) = req_name;
            UI_GU32(0x005d5cd4) = U(msg);                        // Upgrade:OtherUpgradeRequiredDialog:Message's text
        };
        struct { int a, b; } cases[] = {{200, 100}, {300, 20}, {10, 400}, {900, 900}};
        for (auto& k : cases) {
            const char* rn = mk(0, 'r', k.a);
            const char* msg = mk(1, 'm', k.b);
            setup(rn, msg);
            mem_save(g_snap);
            r = fx_run(f, true, {U(c), 0, 10, 10});
            sprintf(m, "UpgradeCatalog::MouseUp, a %d-character upgrade name and a %d-character message: a clean return", k.a, k.b);
            fx_check(fx_clean(f, r), m, &r);
            const std::string e = std::string(rn) + " " + msg;
            sprintf(m, "UpgradeCatalog::MouseUp, %d and %d characters: the box's message is their first 255 characters", k.a, k.b);
            fx_check(g_fx_box_n == 1 && g_fx_box_title == "the upgrade" && g_fx_box_msg == e.substr(0, 255), m);
            o = fx_outside(g_snap, {{c, (uint32_t)sizeof(UpgradeCatalog)}});
            sprintf(m, "UpgradeCatalog::MouseUp, %d and %d characters: nothing written outside the catalog", k.a, k.b);
            fx_check(o == 0, m, 0, o);
        }
        // the boundary: 150 + 1 + 104 = 255
        setup(mk(0, 'r', 150), mk(1, 'm', 104));
        fx_same(f, {U(c), 0, 10, 10}, "UpgradeCatalog::MouseUp, a message of 255 characters");
        fx_check(g_fx_box_n == 2 && g_fx_box_msg.size() == 255, "UpgradeCatalog::MouseUp, a message of 255 characters: shown whole");
    }


    // ---- 10. UpgradeDo: a long Upgrade:Purchased (the dialog answered at once; its frame's copy recorded) ----
    {
        const Ent& f = fx_fn("UpgradeDo");
        FxPatch dlg(F_UIDoDialog, (void*)&fx_UIDoDialog);
        const uint8_t* ups = (const uint8_t*)(uintptr_t)S_UPGRADES;
        for (int n : {256, 700}) {
            fx_reset();
            const char* t = mk(0, 'b', n);
            UI_GU32(0x005d3cfc) = U(t);
            g_fx_dlg_off = 0x36c;                                // the UIDialog at +8: the copy at +0x374, the snapshot at +0x474
            mem_save(g_snap);
            r = fx_run(f, true, {});
            sprintf(m, "UpgradeDo, a %d-character Upgrade:Purchased: a clean return", n);
            fx_check(fx_clean(f, r), m, &r);
            sprintf(m, "UpgradeDo, a %d-character Upgrade:Purchased: its first 255 characters copied, the upgrades' snapshot after "
                       "it intact", n);
            fx_check(first((const char*)g_fx_dlg, 0x100) == first(t, 255) && !memcmp(g_fx_dlg + 0x100, ups, 0x100), m);
            o = fx_outside(g_snap, {{(void*)(uintptr_t)S_UP_NAMES, 0x400}, {(void*)(uintptr_t)S_UP_DESCS, 0x400}});
            sprintf(m, "UpgradeDo, a %d-character Upgrade:Purchased: nothing written but the translations' tables", n);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        UI_GU32(0x005d3cfc) = U(mk(0, 'b', 255));
        g_fx_dlg_off = 0x36c;
        fx_same(f, {}, "UpgradeDo, a 255-character Upgrade:Purchased");
    }

    // ---- 11. UpgradeSummary::Draw: long engine / weight lines (the car file's figures set; the drawn texts recorded) ----
    {
        const Ent& f = fx_fn("UpgradeSummary::Draw");
        FxPatch cf(F_CarFileLoad, (void*)&fx_CarFileLoad);
        FxPatch sd(F_UIStyleDraw, (void*)&fx_UIStyleDraw);
        const std::initializer_list<uint32_t> args = {U(W.summary), 0, U(W.screen)};
        const float big = 3.4028235e38f;
        struct { const char* pw; const char* wt; float p, prpm, t, trpm, w; const char* what; } cases[] = {
            {"Pw", "Wt", big, big, 400.0f, 4000.0f, 3000.0f, "a float's 39 digits for the power and its rpm"},
            {0, "Wt", 450.0f, 5200.0f, 400.0f, 4000.0f, 3000.0f, "a 100-character power translation"},
            {"Pw", 0, 450.0f, 5200.0f, 400.0f, big, big, "the torque's rpm and a weight of 39 digits, a 60-character weight translation"},
        };
        for (auto& c : cases) {
            fx_reset();
            const char* pw = c.pw ? c.pw : mk(0, 'P', 100);
            const char* wt = c.wt ? c.wt : mk(1, 'W', 60);
            UI_GU32(0x005d3804) = U(pw); UI_GU32(0x005d382c) = U("Tq"); UI_GU32(0x005d37f4) = U("hp");
            UI_GU32(0x005d3794) = U("lb-ft"); UI_GU32(0x005d3cd4) = U("rpm"); UI_GU32(0x005d374c) = U(wt);
            g_fx_cd[0] = c.p; g_fx_cd[1] = c.prpm; g_fx_cd[2] = c.t; g_fx_cd[3] = c.trpm; g_fx_cd[4] = c.w;
            mem_save(g_snap);
            r = fx_run(f, true, args);
            sprintf(m, "UpgradeSummary::Draw, %s: a clean return", c.what);
            fx_check(fx_clean(f, r), m, &r);
            char e[3][0x400];
            sprintf(e[0], "%s %1.0f %s @ %1.0f %s", pw, (double)c.p, "hp", (double)c.prpm, "rpm");
            sprintf(e[1], "%s %1.0f %s @ %1.0f %s", "Tq", (double)c.t, "lb-ft", (double)c.trpm, "rpm");
            const float kf = *(const float*)(uintptr_t)0x004dfc64;
            sprintf(e[2], "%s %1.0f %s", wt, ((double)c.w * (double)kf) * (double)1.0f, "lb");
            for (int k = 0; k < 3; k++) {
                bool found = false;
                for (auto& d : g_fx_draws) found |= d == first(e[k], 79);
                sprintf(m, "UpgradeSummary::Draw, %s: line %d is the first 79 characters of \"%.50s...\" (%u)", c.what, k, e[k],
                        (unsigned)strlen(e[k]));
                fx_check(found, m);
            }
            o = fx_outside(g_snap, {{(void*)(uintptr_t)S_GX_CANVAS, 4}});
            sprintf(m, "UpgradeSummary::Draw, %s: nothing written but the current canvas", c.what);
            fx_check(o == 0, m, 0, o);
        }
        // the boundary: a power line of 79 characters ("<61> 450 hp @ 5200 rpm")
        fx_reset();
        UI_GU32(0x005d3804) = U(mk(0, 'P', 61)); UI_GU32(0x005d382c) = U("Tq"); UI_GU32(0x005d37f4) = U("hp");
        UI_GU32(0x005d3794) = U("lb-ft"); UI_GU32(0x005d3cd4) = U("rpm"); UI_GU32(0x005d374c) = U("Wt");
        g_fx_cd[0] = 450.0f; g_fx_cd[1] = 5200.0f; g_fx_cd[2] = 400.0f; g_fx_cd[3] = 4000.0f; g_fx_cd[4] = 3000.0f;
        fx_same(f, args, "UpgradeSummary::Draw, a power line of 79 characters");
    }

    // ---- 12. UpgradeCatalog::Draw: a long description and long lines (the drawn texts recorded) ----
    {
        const Ent& f = fx_fn("UpgradeCatalog::Draw");
        FxPatch sd(F_UIStyleDraw, (void*)&fx_UIStyleDraw);
        UpgradeCatalog* c = W.catalog;
        const std::initializer_list<uint32_t> args = {U(c), 0, U(W.screen)};
        static char desc[0x1100];
        auto words = [](int n) {
            for (int i = 0; i < n; i++) desc[i] = i % 6 == 5 ? ' ' : (char)('a' + i % 23);
            desc[n] = 0;
            return (const char*)desc;
        };
        auto setup = [&](bool allowed, const char* d, const char* price, const char* cls, const char* avail, const char* req,
                         const char* rn) {
            fx_reset();
            c->x = 0; c->y = 0; c->w = 500; c->h = 400; c->axis.pos = 0; c->count = 1; c->index[0] = 1; c->stamps[0] = 0;
            c->hot = -1; c->pressed = 0;
            CshUpgrade* u1 = W.set->items[1];
            u1->req_class = allowed ? 0 : 9;
            u1->price = 1234;
            strcpy(u1->requires, W.upg_names[2]);
            W.set->count = W.nupg;
            uint8_t* info = (uint8_t*)(uintptr_t)S_INFO;
            info[0x1c + 1] = 0;
            *(int32_t*)(info + 0x124) = 1;
            UI_GP(const char, S_UP_NAMES + 4) = "the upgrade";
            UI_GP(const char, S_UP_NAMES + 8) = rn;
            UI_GP(const char, S_UP_DESCS + 4) = d;
            UI_GU32(0x005d5c74) = U(price); UI_GU32(0x005d5c84) = U(cls); UI_GU32(0x005d5ca4) = U(avail); UI_GU32(0x005d5cb4) = U(req);
        };
        for (int allowed = 0; allowed < 2; allowed++) {
            const char* d = words(4095);
            const char* pr = mk(0, 'p', 5000);
            const char* cl = mk(1, 'k', 5000);
            const char* av = mk(2, 'a', 5000);
            const char* rq = mk(3, 'q', 5000);
            const char* rn = mk(4, 'n', 5000);
            setup(allowed != 0, d, pr, cl, av, rq, rn);
            mem_save(g_snap);
            r = fx_run(f, true, args);
            const char* what = allowed ? "allowed" : "not allowed";
            sprintf(m, "UpgradeCatalog::Draw, a 4095-character description and 5000-character translations (%s): a clean return", what);
            fx_check(fx_clean(f, r), m, &r);
            const std::string e_price = first((std::string(pr) + ": $1234").c_str(), 4095);
            const std::string e_state = allowed ? first(av, 4095) : first((std::string("Rookie ") + cl).c_str(), 4095);
            const std::string e_req = first((std::string(rq) + " " + rn).c_str(), 4095);
            bool fd = false, fp_ = false, fs = false, fr = false;
            for (size_t k = 0; k < g_fx_draws.size(); k++) {
                const std::string& t = g_fx_draws[k];
                if (g_fx_draw_style[k] == 0x13 && t.size() == 4095) {  // the description, wrapped (spaces to newlines) whole
                    bool same = true;
                    for (size_t i = 0; i < t.size() && same; i++) same = t[i] == d[i] || (d[i] == ' ' && t[i] == '\n');
                    fd |= same;
                }
                fp_ |= t == e_price;
                fs |= t == e_state;
                fr |= t == e_req;
            }
            sprintf(m, "UpgradeCatalog::Draw (%s): the description drawn whole, the price, state and requirement lines their first "
                       "4095 characters (%d %d %d %d)", what, fd, fp_, fs, fr);
            fx_check(fd && fp_ && fs && fr, m);
            o = fx_outside(g_snap, {{(void*)(uintptr_t)S_GX_CANVAS, 4}, {(void*)(uintptr_t)S_MONEY, 0x40}});
            sprintf(m, "UpgradeCatalog::Draw (%s): nothing written but the current canvas and the money text", what);
            fx_check(o == 0, m, 0, o);
        }
        // the boundaries, at the original's own 2 KB: a description and lines of 2047 characters
        for (int allowed = 0; allowed < 2; allowed++) {
            setup(allowed != 0, words(2047), mk(0, 'p', 2040), mk(1, 'k', 2040), mk(2, 'a', 2047), mk(3, 'q', 1000), mk(4, 'n', 1046));
            fx_same(f, args, allowed ? "UpgradeCatalog::Draw, texts of 2047 characters (allowed)" : "UpgradeCatalog::Draw, texts of 2047 characters (not allowed)");
        }
    }

    printf("the fix build: %d directed checks (%d boundary cases on both), %d failed\n", g_fx_n, g_fx_same_n, g_fx_bad);
    return g_fx_bad;
}
#endif

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
    char* s = strstr(exe, "\\test\\world_career_shop.cpp");
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
        {F_FileOpen, (void*)&stub_FileOpen}, {F_FileClose, (void*)&stub_FileClose},
        {F_ClipboardGetText, (void*)&stub_ClipboardGetText},
        // this group's callees
        {F_stricmp, (void*)&stub_stricmp}, {F_LocaleMoney, (void*)&stub_LocaleMoney},
        {F_LocaleFormatShortDate, (void*)&stub_LocaleFormatShortDate},
        {F_CareerGetInfo, (void*)&stub_CareerGetInfo}, {F_CareerGetSeason, (void*)&stub_CareerGetSeason},
        {F_CareerGetUpgradeSet, (void*)&stub_CareerGetUpgradeSet}, {F_CareerIsTestMode, (void*)&stub_CareerIsTestMode},
        {F_CareerGetCarName, (void*)&stub_CareerGetCarName}, {F_CareerGetClassName, (void*)&stub_CareerGetClassName},
        {F_CareerLog, (void*)&stub_CareerLog}, {F_PaintKitDo, (void*)&stub_PaintKitDo}, {F_Xlate, (void*)&stub_Xlate},
        {F_TimeGetTimeOfDay, (void*)&stub_TimeGetTimeOfDay}, {F_CarFileMakeDefaultSetup, (void*)&stub_CarFileMakeDefaultSetup},
        {F_CarFileLoad, (void*)&stub_CarFileLoad}, {F_Random, (void*)&stub_Random},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    patch_gen(std::make_index_sequence<N_GEN>());
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    build_world();
    mem_save(g_pristine);
    printf("world: %u $E initialisers run, %d widgets in the window, %u KB of heap used, %d generic stubs\n",
           (unsigned)g_setup_e.size(), (int)W.widgets.size(), (HS->heap_next - A_HEAP) / 1024, (int)N_GEN);

    int dup = 0;
    for (int i = 0; i < g_nfns; i++)
        for (int j = i + 1; j < g_nfns; j++)
            if (g_fns[i].v10 == g_fns[j].v10 || !strcmp(g_fns[i].name, g_fns[j].name)) {
                printf("  listed twice: %08x %s / %08x %s\n", g_fns[i].v10, g_fns[i].name, g_fns[j].v10, g_fns[j].name);
                dup++;
            }
    static Footprint fp;
    long long checks = 0, poisoned_checks = 0, log_words = 0;
    int differ = 0, fp_bad = 0, faults = 0, pure_n = 0, replay_n = 0, changed_fns = 0, still_fns = 0, bad_fns = 0;
    long long both_fault = 0, fp_checked = 0;
    int fixed_rounds = 0, fixed_bad = 0;                           // (the fix build: rounds kept out, a fixed case)
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0, fn_fpck = 0, fn_fixed = 0;
        int cover[N_COVER] = {};
        const int nr = is_modal(f.name) ? rounds : rounds * 10;
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            if (poisoned) poison(g_pristine);
            randomize_world();
            randomize_statics();
            random_script(is_modal(f.name) ? 30 : 6);
            uint32_t words[72];
            make_args(f, words);
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            mem_save(g_snap);
            g_overran = false;
            const Result ro = run(f, false, words);
#if CAREER_FIXES
            if (g_overran && !strcmp(f.name, "UpgradeSummary::Draw")) {   // a fixed case: the rewrite clean (or faulting
                mem_load(g_snap);                                          // just where the original did)
                const Result rf = run(f, true, words);
                fixed_rounds++;
                fn_fixed++;
                if (rd == 0) { pure_n += fp.pure; replay_n += fp.replay_only != 0; }
                if (rf.fault && !(ro.fault && ro.code == rf.code && ro.eip == rf.eip)) {
                    printf("  %08x %s: round %d, a fixed case: the rewrite faulted (%08x at %08x)\n", f.v10, f.name, rd, rf.code, rf.eip);
                    fixed_bad++;
                    fn_bad = true;
                }
                continue;
            }
#endif
            const bool changed = mem_diff(g_snap) != 0;
            fn_changed |= changed;
            if (!fp.replay_only && !ro.fault) {
                fn_fpck++;
                fp_checked++;
                uint32_t out = 0;
                auto covered = [&](const uint8_t* p) {
                    for (int k = 0; k < fp.n; k++) if (p >= (uint8_t*)fp.r[k].p && p < (uint8_t*)fp.r[k].p + fp.r[k].n) return true;
                    return false;
                };
                // (a 4 KB block at a time: most are unchanged)
                for (uint32_t b = 0; b < DATA_BYTES && !out; b += 0x1000) {
                    const uint32_t e = b + 0x1000 < DATA_BYTES ? b + 0x1000 : DATA_BYTES;
                    if (!memcmp(g_snap.data + b, DATA + b, e - b)) continue;
                    for (uint32_t i2 = b; i2 < e && !out; i2++)
                        if (g_snap.data[i2] != DATA[i2] && !covered(DATA + i2)) out = 0x004e1000 + i2;
                }
                for (uint32_t b = 0; b < ARENA_BYTES && !out; b += 0x1000) {
                    if (!memcmp(g_snap.arena + b, g_arena + b, 0x1000)) continue;
                    for (uint32_t i2 = b < sizeof(HState) ? (uint32_t)sizeof(HState) : b; i2 < b + 0x1000 && !out; i2++)
                        if (g_snap.arena[i2] != g_arena[i2] && !covered(g_arena + i2)) out = U(g_arena + i2);
                }
                if (out) {
                    printf("  FOOTPRINT %08x %s: the original changed %08x outside it (round %d)\n", f.v10, f.name, out, rd);
                    fp_bad++;
                    fn_bad = true;
                }
            }
            mem_save(g_after);
            memcpy(&g_log_orig, &g_log, sizeof(uint32_t) * (1 + (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)));
            for (int c = 0; c < N_COVER; c++)
                for (uint32_t k = 0; k < g_log_orig.n && k < LOG_MAX; k++)
                    if (g_log_orig.w[k] == k_cover[c]) { cover[c]++; break; }
            mem_load(g_snap);
            const Result rn = run(f, true, words);
            checks++;
            fn_checks++;
            poisoned_checks += poisoned;
            log_words += g_log_orig.n;
            if (ro.fault || rn.fault) { faults++; fn_faults++; }
            if (ro.fault && rn.fault) both_fault++;
            if (getenv("VP_DEBUG_LOG")) {
                printf("    round %d: %u logged words:", rd, g_log_orig.n);
                for (uint32_t k = 0; k < g_log_orig.n && k < 48; k++) printf(" %08x", g_log_orig.w[k]);
                printf("\n");
            }
            if ((ro.fault || rn.fault) && getenv("VP_DEBUG_WORLD"))
                printf("    round %d: faulted %d/%d, %08x at %08x / %08x at %08x\n", rd, ro.fault, rn.fault, ro.code, ro.eip, rn.code, rn.eip);
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
                        for (uint32_t k = i >= 8 ? i - 8 : 0; k < i + 3 && k < g_log.n; k++) printf(" %08x/%08x", g_log_orig.w[k], g_log.w[k]);
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
            printf("%08x %-58s %s%s (%d checks, %d with the footprint checked, %d faulted)\n", f.v10, f.name, fn_bad ? "BAD" : "ok",
                   fn_changed ? "" : " (never changed memory)", fn_checks, fn_fpck, fn_faults);
        if (getenv("VP_COVER")) {
            printf("    reached:");
            for (int c = 0; c < N_COVER; c++)
                if (cover[c])
                    printf(" %c%c%c%c %d", (char)(k_cover[c] >> 24), (char)(k_cover[c] >> 16), (char)(k_cover[c] >> 8), (char)k_cover[c], cover[c]);
            printf("\n");
        }
        if (fn_fixed) printf("  %08x %s: %d rounds reached a fixed case (kept out of the comparison; the rewrite clean in each, or "
                             "faulting just where the original did)\n", f.v10, f.name, fn_fixed);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    printf("%d functions (%d pure, %d replay_only in round 0, %d others), %d listed twice; %lld checks (%lld on poisoned .data, "
           "%lld with the footprint checked), %lld logged words compared: %d differ, %d footprint violations, %d checks faulted "
           "(%lld in both, the same way); %d functions bad\n", g_nfns, pure_n, replay_n, g_nfns - pure_n - replay_n, dup, checks,
           poisoned_checks, fp_checked, log_words, differ, fp_bad, faults, both_fault, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    int fix_bad = 0;
#if CAREER_FIXES
    printf("the fix build: %d rounds reached a fixed case (kept out of the comparison), %d where the rewrite faulted\n", fixed_rounds,
           fixed_bad);
    if (!only) fix_bad = directed_fix_tests();
#endif
#if !CAREER_FIXES
    (void)fixed_rounds; (void)fixed_bad;
#endif
    return differ || fp_bad || dup || fix_bad || fixed_bad ? 1 : 0;
}
