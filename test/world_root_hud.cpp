// world_root_hud.cpp -- M3 UI stage, step U3 (group A): every rewrite of hook/root_dash.cpp (dash.obj),
// hook/root_screens.cpp (escape.obj, splash.obj, hack.obj, gxdash.obj, ghost.obj's $E62) and the generated
// hook/root_leftover.cpp (the $E static initialisers of all fourteen root object files, and the bare-`ret` stubs of dash,
// countdwn and hack) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_root_hud.cpp
//        /Fo<dir>\ /Fe<dir>\world_root_hud.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_root_hud.exe [rounds] [seed]      (VP_TRACE=1: one line per function; VP_ONLY=name: just those)
//
// Built as test/world_menu_options.cpp is (its loader, raw call, memory comparison and logs are copied here): out\race_v10.exe
// loaded at 0x400000 (a child process with the range reserved), the three rewrite files included with PORT_FN redefined to
// list each function (its v1.0 address, the rewrite, its calling convention, stack arguments, return, footprint).
// VP_FAITHFUL: the rewrites exactly as the originals.
//
// The world. Once, at start: every root $E static initialiser (the listed ones: the global Xlators, colours, World), the
// game's own DashboardBegin (the dash's fonts, stamps, palettes and canvases, from the stubs) and EscapeMenuBegin; a
// LocaleInfo, a GameOptions, a Deity (a logging vtable: GetNumLaps, CarIsFinished), 16 car messages, two escape menus
// (callbacks that log), a canvas to draw on. This is the pristine state.
//
// Then for EVERY listed function, `rounds` times (default 24; ten times that for one that isn't replay_only): pristine
// restored; half the rounds POISONED (every byte of .data/.bss random but for what's read as a string or followed as a
// pointer); then the world random on top: the 16 CarMgrInfos (types, places, laps, the lap / best / last times, gaps and
// the cars ahead and behind, names) and their messages (rpm, gear -1..7 and now and then past it, speed, the off-track and
// damage flags), floats NaN / infinite / huge (kept under 1e12 where the original formats them into its 64-byte buffers:
// FIX CANDIDATE in root_dash.cpp), the dash's statics (redraw, the dirty mask, what was last drawn -- often equal to the
// car's, so both the unchanged and changed paths run), the Xlator guards (mostly built), the banners' flags, the screen
// size (640x480, 512x384, 320x240, 800x600, 1024x768), the escape menu (none, or one of 1..4 items and its selection), the
// hack flags and factors, the LOD factor, the splash statics; the stubs' world (focus / player car, camera, race state,
// game type, car count, the Deity's answers, race times, clock step, input script, a failing screen grab). Arguments made
// for the function (a canvas, a key, a flag, a menu, stamps and texts or none, timeouts). Snapshot; the ORIGINAL runs from a
// raw call thunk (the stack filled with one pattern after the FPU is set); its results kept, the snapshot restored, the
// REWRITE runs the same way. Compared: every byte of .data/.bss/.idata and the 1 MB arena (a pointer into this thread's
// stack in both counts as equal), the return value, st0 and the x87 depth, the bytes popped, ebx / esi / edi / ebp, faults,
// and the call logs of every stub. Every byte the original changed must lie in the rewrite's footprint (unless it's
// replay_only). The loops (the splashes, wait_key_timeout, SplashDoneLoading) end on their own: the clock runs, a
// watchdog after 300 reads of it feeds a key and a click. The x87 runs at 24 or 53 bits (alternating rounds).
//
// Stubbed (a jump to a logger; state in the arena so both passes see the same): memory, the logs, the unsafe-module lock,
// atexit, the CRT's fatal paths, input and the clock, Win32Idle, TaskSleep, the 2D calls (stamps, fonts, gxFontPrintf --
// its format's arguments logged --, palettes, canvases, clip, rect / line / triangle / paste / text, the screen grab and
// flip), the 3D alpha rectangles and mrIsEnabled / Enable / Disable, UIStyleDraw / Width, UIAddDynamicStyle (the
// description logged) / UIRemoveStyle, Xlator::xlate, sprintf (the game's formats, by the CRT's), LocaleConvertNumeric,
// PhysicsTimeString (writes its buffer), TelemetrySetAddMeter, VidWasLost, the World and CarMgr queries, RecordGetRaceTime,
// PhysicsGetTime, AICarCount, GetCarFileNumber and the options. The game's own code everywhere else: Xlator's constructor,
// MainIsDone / MainSetCamera, GhostCarIncognito, MultiEnabled, PhysicsIsPaused, and every function of these objects (each
// rewrite is checked against its original with the same callees).
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
static Ent g_fns[2048];
static int g_nfns;
struct EntReg { EntReg(const Ent& e) { if (g_nfns < 2048) g_fns[g_nfns++] = e; } };
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

#include "../hook/root_dash.cpp"
#include "../hook/root_screens.cpp"
#include "../hook/root_leftover.cpp"

using namespace uit;
using namespace rhud;

// ---- random numbers --------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x6a09e667u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int irange(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits_(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float range(float a, float b) { return a + (b - a) * uni(); }
// a float the original may format: NaN, infinities, zero, denormals, small, up to 1e12
static float wildf() {
    switch (rnd() % 10) {
    case 0: return bitsf(0x7fc00000u);
    case 1: return bitsf(0xffc00000u | (rnd() & 0x3fffff));
    case 2: return (rnd() & 1) ? INFINITY : -INFINITY;
    case 3: return 0.0f;
    case 4: return bitsf(rnd() & 0x807fffff);
    case 5: return range(-1e12f, 1e12f);
    case 6: return range(-1.0f, 1.0f);
    default: return range(-200.0f, 400.0f);
    }
}
// any float at all (for the dial, the LOD factor: never formatted past its buffer)
static float anyf() { return chance(30) ? bitsf(rnd()) : wildf(); }

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
struct HState {
    uint32_t heap_next;
    int32_t time, tstep, frames, frame_limit;
    int32_t nkeys, key_pos, keys[16];
    int32_t nmouse, mouse_pos, mouse[16];            // event types
    int32_t forced_keys, forced_mouse;
    int32_t pal_next, canvas_next, grab_fail_at, style_next;
    uint32_t opt_seed;
    int32_t focus, player, camera, race_state, game_type, count, ai_count, numlaps, carfile;
    uint8_t lost, pad[3];
    uint32_t phys_time, race_time[16], finished, mr_mask;
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
    if ((uint64_t)a + n > 0xffffffffull) return false;          // (a + n wrapping to a small number passed every range)
    if (a >= (uintptr_t)g_arena && a + n <= (uintptr_t)g_arena + ARENA_BYTES) return true;
    if (a >= 0x401000 && a + n <= 0x5d9000) return true;
    if (a >= g_self_lo && a + n <= g_self_hi) return true;
    if (on_stack((uint32_t)a) && on_stack((uint32_t)(a + n - 1))) {
        // the stack's reserved range: only its committed pages (a garbage pointer can aim below them)
        MEMORY_BASIC_INFORMATION mi;
        for (uintptr_t pg = a & ~(uintptr_t)0xfff; pg <= ((a + n - 1) & ~(uintptr_t)0xfff); pg += 0x1000)
            if (!VirtualQuery((void*)pg, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                return false;
        return true;
    }
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
static void L_canvas() {
    const gxCanvas* c = UI_GP(gxCanvas, S_GX_CANVAS);
    L(P(c));
    if (c && readable(c, sizeof(gxCanvas))) { L((uint32_t)c->cx0); L((uint32_t)c->cy0); L((uint32_t)c->cx1); L((uint32_t)c->cy1); }
}
static uint32_t hash_str(const char* s) { uint32_t h = 2166136261u; if (!s) return 0; for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }
// a format's arguments: strings by their text, doubles by their two words, the rest as words
static void L_va(const char* fmt, const uint32_t* w) {
    L(P(fmt));
    if (!readable(fmt, 1)) return;
    LS(fmt);
    int nw = 0;
    for (const char* p = fmt; readable(p, 1) && *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.*lh", *p)) { if (*p == '*') L(w[nw++]); p++; }
        if (!*p) break;
        if (*p == 's') { LS((const char*)(uintptr_t)w[nw++]); continue; }
        if (strchr("eEfgG", *p)) { L(w[nw]); L(w[nw + 1]); nw += 2; continue; }
        L(on_stack(w[nw]) ? 'STAK' : w[nw]);
        nw++;
        if (nw > 12) break;
    }
}

// ---- stubs: memory, logs, sync ----------------------------------------------------------------------------------------------
static void* __cdecl stub_MemAlloc(int n) {
    L('ALOC'); L((uint32_t)n);
    uint32_t a = (HS->heap_next + 7) & ~7u;
    if (a < A_HEAP || a + (uint32_t)(n > 0 ? n : 0) > ARENA_BYTES - 0x100 || n < 0 || n > 0x40000) { L('OOM '); return 0; }
    HS->heap_next = a + (uint32_t)n;
    return g_arena + a;
}
static void __cdecl stub_delete(void* p) { L('DEL '); L(P(p)); }
static void __cdecl stub_LogReport(const char* fmt, ...) { L('LOGR'); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); }
static void __cdecl stub_LogPanic(const char* fmt, ...) { L('PANC'); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); }
static int __cdecl stub_SingleBegin(const char* s) { L('SBEG'); LS(s); return 3; }
static void __cdecl stub_SingleEnd(int h, const char* f, int l) { L('SEND'); L((uint32_t)h); L(P(f)); L((uint32_t)l); }
static void __cdecl stub_SingleEnter(int h, const char* f, int l) { L('SENT'); L((uint32_t)h); L(P(f)); L((uint32_t)l); }
static void __cdecl stub_SingleLeave(int h, const char* f, int l) { L('SLEV'); L((uint32_t)h); L(P(f)); L((uint32_t)l); }
static int __cdecl stub_atexit(uint32_t fn) { L('ATEX'); L(fn); return 0; }
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }
static int __cdecl stub_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}

// ---- stubs: input, time ---------------------------------------------------------------------------------------------------------
static int32_t __cdecl stub_PTimeNow() {
    L('TIME');
    HS->frames++;
    if (HS->frames == HS->frame_limit) { HS->forced_keys = 1; HS->forced_mouse = 1; L('WDOG'); }
    HS->time += HS->frames > HS->frame_limit ? 0x1000000 : HS->tstep;
    return HS->time;
}
struct MouseEv { int32_t type, x, y, state; };
static uint8_t __cdecl stub_MouseGetEvent(MouseEv* ev) {
    L('MGEV');
    int32_t t = -1;
    if (HS->forced_mouse) { HS->forced_mouse = 0; t = 0; }
    else if (HS->mouse_pos < HS->nmouse) t = HS->mouse[HS->mouse_pos++];
    if (t < 0) return 0;
    ev->type = t; ev->x = 5; ev->y = 7; ev->state = 0;
    L((uint32_t)t);
    return 1;
}
static uint8_t __cdecl stub_KeyHit() { L('KHIT'); return (HS->forced_keys > 0 || HS->key_pos < HS->nkeys) ? 1 : 0; }
static uint16_t __cdecl stub_KeyGet() {
    L('KGET');
    if (HS->forced_keys > 0) { HS->forced_keys--; return 0x20; }
    if (HS->key_pos < HS->nkeys) return (uint16_t)HS->keys[HS->key_pos++];
    return 0;
}
static void __cdecl stub_MouseClear() { L('MCLR'); }
static void __cdecl stub_KeyClear() { L('KCLR'); }
static void __cdecl stub_Win32Idle() { L('IDLE'); }
static void __cdecl stub_TaskSleep(int ms) { L('SLEP'); L((uint32_t)ms); }

// ---- stubs: the 2D calls ----------------------------------------------------------------------------------------------------------
struct Fake { uint32_t magic; char name[32]; int32_t w, h, count, asc, desc, cw; };
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
    f->w = 12 + (int)(h % 200); f->h = 10 + (int)((h >> 8) % 100); f->count = 1 + (int)((h >> 16) % 6);
    f->asc = 7 + (int)(h % 4); f->desc = 2 + (int)((h >> 4) % 3); f->cw = 5 + (int)((h >> 12) % 4);
    return f;
}
static int32_t fake_val(const void* p, int which) {
    for (int i = 0; i < g_nfakes; i++)
        if (p == &g_fakes[i]) { const int32_t v[] = {g_fakes[i].w, g_fakes[i].h, g_fakes[i].count, g_fakes[i].asc, g_fakes[i].desc, g_fakes[i].cw}; return v[which]; }
    const uint32_t h = (uint32_t)(uintptr_t)p * 2654435761u;
    const int32_t v[] = {(int32_t)(8 + h % 20), (int32_t)(6 + (h >> 8) % 20), (int32_t)(1 + (h >> 16) % 4), 8, 2, 6};
    return v[which];
}
static void* __cdecl stub_gxGetStamp(const char* name) { L('GSTP'); LS(name); return fake_for(name, 'STMP'); }
static void __cdecl stub_gxForgetStamp(void* s) { L('FSTP'); L(P(s)); }
static int32_t __cdecl stub_gxStampWidth(const void* s) { L('STW '); L(P(s)); return fake_val(s, 0); }
static int32_t __cdecl stub_gxStampHeight(const void* s) { L('STH '); L(P(s)); return fake_val(s, 1); }
static void __cdecl stub_gxDrawStamp(const void* s, int32_t x, int32_t y, int32_t frame, const void* pal) {
    L('DSTP'); L(P(s)); L((uint32_t)x); L((uint32_t)y); L((uint32_t)frame); L(P(pal)); L_canvas();
}
static void* __cdecl stub_gxFontGet(const char* name) { L('FGET'); LS(name); return fake_for(name, 'FONT'); }
static void __cdecl stub_gxFontForget(void* f) { L('FFGT'); L(P(f)); }
static int32_t __cdecl stub_gxFontHeight(const void* f) { L('FHT '); L(P(f)); return fake_val(f, 3) + fake_val(f, 4); }
static int32_t __cdecl stub_gxFontStringWidth(const void* f, const char* s) {
    L('FSW '); L(P(f)); LS(s);
    return readable(s, 1) ? (int32_t)strlen(s) * fake_val(f, 5) : 0;
}
static void __cdecl stub_gxFontPrintf(const void* f, const void* pal, uint32_t flags, int32_t x, int32_t y, const char* fmt, ...) {
    L('FPRF'); L(P(f)); L(P(pal)); L(flags); L((uint32_t)x); L((uint32_t)y);
    va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap);
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
static void __cdecl stub_gxTriangle(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t g, uint32_t col) {
    L('TRI '); L(a); L(b); L(c); L(d); L(e); L(g); L(col); L_canvas();
}
static void __cdecl stub_gxPaste(const gxCanvas* src, int32_t x, int32_t y) { L('PAST'); L(P(src)); L((uint32_t)x); L((uint32_t)y); L_canvas(); }
static void __cdecl stub_gxText(int32_t x, int32_t y, const char* s, uint32_t col) { L('TEXT'); L((uint32_t)x); L((uint32_t)y); LS(s); L(col); L_canvas(); }
static void __cdecl stub_gxSetClip(gxCanvas* c, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    L('CLIP'); L(P(c)); L(x0); L(y0); L(x1); L(y1);
    if (readable(c, sizeof(gxCanvas))) { c->cx0 = x0; c->cy0 = y0; c->cx1 = x1; c->cy1 = y1; }
}
static void __cdecl stub_gxRestoreClip(gxCanvas* c) {
    L('RCLP'); L(P(c));
    if (readable(c, sizeof(gxCanvas))) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }
}
static void __cdecl stub_gxAllocCanvas(gxCanvas* c, int32_t w, int32_t h) {
    L('ACNV'); L(P(c)); L((uint32_t)w); L((uint32_t)h);
    c->format = 4; c->flags = 0; c->pixels = (uint8_t*)(uintptr_t)(0x7c000000u + (uint32_t)(HS->canvas_next++) * 0x100000u);
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
// the 3D side
static void __cdecl stub_mrBeginAlphaRects() { L('MBAR'); }
static void __cdecl stub_mrEndAlphaRects() { L('MEAR'); }
static void __cdecl stub_mrDrawAlphaRect(int32_t a, int32_t b, int32_t c, int32_t d, uint32_t col) { L('MDAR'); L(a); L(b); L(c); L(d); L(col); }
static uint8_t __cdecl stub_mrIsEnabled(int32_t f) { L('MISE'); L((uint32_t)f); return (HS->mr_mask & (uint32_t)f) ? 1 : 0; }
static void __cdecl stub_mrEnable(int32_t f) { L('MENA'); L((uint32_t)f); HS->mr_mask |= (uint32_t)f; }
static void __cdecl stub_mrDisable(int32_t f) { L('MDIS'); L((uint32_t)f); HS->mr_mask &= ~(uint32_t)f; }
// the toolkit
static void __cdecl stub_UIStyleDraw(int32_t style, int32_t x, int32_t y, const char* s, uint32_t st) {
    L('USDR'); L((uint32_t)style); L((uint32_t)x); L((uint32_t)y); LS(s); L(st); L_canvas();
}
static int32_t __cdecl stub_UIStyleWidth(int32_t style, const char* s) { L('USWD'); L((uint32_t)style); LS(s); return readable(s, 1) ? (int32_t)strlen(s) * 7 : 3; }
static int32_t __cdecl stub_UIAddDynamicStyle(const uint32_t* d) {
    L('UADS'); L(P(d));
    for (int i = 0; i < 13; i++) L(d[i]);
    return 20 + HS->style_next++;
}
static void __cdecl stub_UIRemoveStyle(int32_t s) { L('URMS'); L((uint32_t)s); }

// ---- stubs: text, the world ----------------------------------------------------------------------------------------------------
// An Xlator's translation: one of a few fixed texts, by its key. Not the key's own bytes: in a poisoned round a key's
// string (in .data) is random, and TheRealDashboardDraw passes some translations to gxFontPrintf as the format -- a '%' in
// one makes gxFontPrintf's stub log "arguments" that were never passed (the callers' stack past the real ones, which
// differs between the two passes' frames). The game's translations have no '%' (FIX CANDIDATE in root_dash.cpp).
static const char* const k_xl_texts[4] = {"Resume", "Reset car? Press R", "Damaged", "Race results"};
static uint32_t xl_text(uint32_t key) { return (uint32_t)(uintptr_t)k_xl_texts[(key * 2654435761u) >> 30]; }
static void __fastcall stub_xlate(uint32_t* xl_, int) {
    L('XLAT'); L(P(xl_));
    xl_[1] = xl_text(xl_[0]);
    xl_[2] = UI_GU32(S_XLATOR_COOKIE);
}
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    L('SPRF'); L(P(buf));
    va_list ap;
    va_start(ap, fmt);
    L_va(fmt, (const uint32_t*)ap);
    const int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    LS(buf);
    return r;
}
static void __cdecl stub_LocaleConvertNumeric(char* s) { L('LCNV'); LS(s); for (char* p = s; *p; p++) if (*p == '.') *p = ','; }
static const char* __cdecl stub_PhysicsTimeString(uint32_t t, uint32_t hundredths) {
    L('PTST'); L(t); L(hundredths & 0xff);
    char* b = (char*)(uintptr_t)S_TIME_STRING;
    sprintf(b, "T%08x.%u", t, hundredths & 0xff);
    return b;
}
static uint8_t __cdecl stub_TelemetrySetAddMeter(int32_t set, const char* name, void* p) { L('TELM'); L((uint32_t)set); LS(name); L(P(p)); return 1; }
static uint8_t __cdecl stub_VidWasLost() { L('VLST'); return HS->lost; }
static int32_t __cdecl stub_WorldGetFocusCar() { L('WFOC'); return HS->focus; }
static int32_t __cdecl stub_WorldGetPlayerCar() { L('WPLY'); return HS->player; }
static int32_t __cdecl stub_WorldGetCameraType() { L('WCAM'); return HS->camera; }
static int32_t __cdecl stub_WorldGetRaceState() { L('WRST'); return HS->race_state; }
static uint8_t* g_gameopts;
static void* __cdecl stub_WorldGameOptions() { L('WGOP'); *(int32_t*)(g_gameopts + 0x18) = HS->game_type; return g_gameopts; }
static int32_t __cdecl stub_CarMgrCount() { L('CMCN'); return HS->count; }
static void* __cdecl stub_CarMgrGetInfo(int32_t car) { L('CMGI'); L((uint32_t)car); return (void*)(uintptr_t)(S_CARMGR + (uint32_t)(car & 15) * 0x170u); }
static float __cdecl stub_RecordGetRaceTime(int32_t car) { L('RGRT'); L((uint32_t)car); return bitsf(HS->race_time[car & 15]); }
static float __cdecl stub_PhysicsGetTime() { L('PGTM'); return bitsf(HS->phys_time); }
static int32_t __cdecl stub_AICarCount() { L('AICN'); return HS->ai_count; }
static int32_t __cdecl stub_GetCarFileNumber(const char* s) { L('GCFN'); LS(s); return HS->carfile; }
static uint32_t opt_h(const char* sec, const char* key) { return (hash_str(readable(sec, 1) ? sec : "") * 31u) ^ hash_str(readable(key, 1) ? key : "") ^ HS->opt_seed; }
static void __cdecl stub_OptionsGetF(const char* sec, const char* key, uint32_t* v) {
    L('OGF '); LS(sec, 32); LS(key, 64); L(P(v));
    const uint32_t h = opt_h(sec, key);
    if (!(h & 3)) { L('MISS'); L(*v); return; }
    static const uint32_t vals[] = {0x3f800000u, 0x3f000000u, 0x40000000u, 0xbf800000u, 0x7fc00000u, 0x7f800000u, 0, 0x3dcccccdu};
    *v = vals[(h >> 3) & 7];
}
static void __cdecl stub_OptionsGetI(const char* sec, const char* key, int32_t* v) {
    L('OGI '); LS(sec, 32); LS(key, 64); L(P(v));
    const uint32_t h = opt_h(sec, key);
    if (!(h & 3)) { L('MISS'); L((uint32_t)*v); return; }
    *v = (int32_t)((h >> 3) % 9) - 1;
}
static void __cdecl stub_OptionsGetB(const char* sec, const char* key, uint8_t* v) {
    L('OGB '); LS(sec, 32); LS(key, 64); L(P(v));
    const uint32_t h = opt_h(sec, key);
    if (!(h & 3)) { L('MISS'); L(*v); return; }
    *v = (uint8_t)((h >> 3) & 1);
}
static void __cdecl stub_OptionsSetI(const char* sec, const char* key, int32_t v) { L('OSI '); LS(sec, 32); LS(key, 64); L((uint32_t)v); }
// the Deity
static int32_t __fastcall deity_GetNumLaps(void* self, int) { L('DNLP'); L(P(self)); return HS->numlaps; }
static uint8_t __fastcall deity_CarIsFinished(void* self, int, int32_t car) { L('DFIN'); L(P(self)); L((uint32_t)car); return (uint8_t)((HS->finished >> (car & 31)) & 1); }
static void* g_deity_vtbl[20];
// the escape menus' callbacks
static void __cdecl esc_begin() { L('EBEG'); }
static void __cdecl esc_end() { L('EEND'); }
static void __cdecl esc_chosen(int32_t id) { L('ECHO'); L((uint32_t)id); }

// ---- the raw call ------------------------------------------------------------------------------------------------------------
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
    if (getenv("VP_FAULT_ADDR") && e->ExceptionRecord->NumberParameters >= 2)
        printf("      (fault: %s %08x)\n", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
               (uint32_t)e->ExceptionRecord->ExceptionInformation[1]);
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}
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
    gxCanvas* canvas;                                // a canvas to draw on
    uint8_t* locale;                                 // LocaleInfo
    void** deity;                                    // {vtable}
    CarMsg* msgs;                                    // 16
    EscapeMenu* menus;                               // 2
    char* texts;                                     // strings (0x400)
    char* names[8];
};
static World W;
static const char* k_names[8] = {"viper", "gts", "rt10", "acr", "jeep", "willys", "ghost", "a longer name!"};

static void build_world() {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    HS->tstep = 16;
    UI_G32(S_SCREEN_W) = 640;
    UI_G32(S_SCREEN_H) = 480;
    W.canvas = (gxCanvas*)wv(sizeof(gxCanvas));
    W.canvas->format = 4; W.canvas->pixels = (uint8_t*)0x7d000000; W.canvas->w = 640; W.canvas->h = 480; W.canvas->pitch = 1280;
    W.canvas->cx1 = 640; W.canvas->cy1 = 480;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.canvas;
    W.texts = (char*)wv(0x400);
    for (int i = 0; i < 8; i++) { W.names[i] = W.texts + 0x40 * i; strcpy(W.names[i], k_names[i]); }
    strcpy(W.texts + 0x200, "mph");
    strcpy(W.texts + 0x210, "km");
    strcpy(W.texts + 0x220, "Resume");
    strcpy(W.texts + 0x230, "Restart");
    strcpy(W.texts + 0x240, "Quit race");
    strcpy(W.texts + 0x250, "Options");
    strcpy(W.texts + 0x260, "flag.stp");
    W.locale = (uint8_t*)wv(0x40);
    UI_GP(void, S_LOCALE) = W.locale;
    g_gameopts = (uint8_t*)wv(0x40);
    W.deity = (void**)wv(16);
    for (int i = 0; i < 20; i++) g_deity_vtbl[i] = (void*)&stub_crt_fatal;
    g_deity_vtbl[VO_Deity_GetNumLaps / 4] = (void*)&deity_GetNumLaps;
    g_deity_vtbl[VO_Deity_CarIsFinished / 4] = (void*)&deity_CarIsFinished;
    W.deity[0] = g_deity_vtbl;
    UI_GP(void, S_DEITY) = W.deity;
    W.msgs = (CarMsg*)wv(sizeof(CarMsg) * 16 + 0x40);
    W.menus = (EscapeMenu*)wv(sizeof(EscapeMenu) * 2 + 0x40);
    for (int i = 0; i < 16; i++) ((CarMgrInfo*)(uintptr_t)(S_CARMGR + 0x170u * i))->msg = &W.msgs[i];
    // the root's static initialisers (every listed one), the dash, the escape menu's lock
    for (int i = 0; i < g_nfns; i++)
        if (g_fns[i].name[0] == '$') call_orig(g_fns[i].v10, 0, {});
    call_orig(F_DashboardBegin, 0, {});
    call_orig(F_EscapeMenuBegin, 0, {});
    HS->frame_limit = 300;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
static void randomize_world() {
    static const int32_t sizes[5][2] = {{640, 480}, {512, 384}, {320, 240}, {800, 600}, {1024, 768}};
    const int s = chance(50) ? 0 : irange(0, 4);
    UI_G32(S_SCREEN_W) = sizes[s][0];
    UI_G32(S_SCREEN_H) = sizes[s][1];
    UI_GP(gxCanvas, S_GX_CANVAS) = chance(50) ? W.canvas : (gxCanvas*)(uintptr_t)S_CANVAS;
    UI_GP(void, S_LOCALE) = W.locale;
    UI_GP(void, S_DEITY) = W.deity;
    // the locale
    *(uint32_t*)(W.locale + 8) = U32(W.texts + 0x210);
    *(float*)(W.locale + 0xc) = chance(80) ? (chance(50) ? 1.0f : 0.000621371f) : range(-3.0f, 3.0f);
    *(uint32_t*)(W.locale + 0x18) = U32(W.texts + 0x200);
    *(float*)(W.locale + 0x1c) = chance(80) ? (chance(50) ? 3.6f : 2.2369363f) : range(-3.0f, 3.0f);
    // the cars
    for (int i = 0; i < 16; i++) {
        CarMgrInfo* c = (CarMgrInfo*)(uintptr_t)(S_CARMGR + 0x170u * i);
        c->type = chance(15) ? 3 : irange(0, 2);
        strcpy(c->name, k_names[rnd() & 7] + (chance(10) ? 1 : 0));
        c->msg = &W.msgs[i];
        CarInfo* ri = &c->info;
        ri->lap = chance(10) ? irange(-3, 100) : irange(0, 5);
        ri->place = chance(10) ? irange(-2, 20) : irange(1, 8);
        ri->lap_time = chance(40) ? range(0.0f, 6.0f) : wildf();
        ri->best = wildf(); ri->last = chance(30) ? range(0.5f, 90.0f) : wildf();
        ri->best_speed = wildf();
        ri->gap_behind = chance(50) ? range(-1.0f, 40.0f) : wildf();
        ri->gap_ahead = chance(50) ? range(-1.0f, 40.0f) : wildf();
        ri->behind = irange(0, 15); ri->ahead = irange(0, 15);
        CarMsg* m = &W.msgs[i];
        m->rpm = fbits_(chance(70) ? range(0.0f, 9000.0f) : anyf());
        m->gear = chance(95) ? irange(-1, 7) : irange(-4, 12);
        m->speed = chance(70) ? range(-10.0f, 120.0f) : wildf();
        m->flags = (uint8_t)rnd();
        HS->race_time[i] = fbits_(chance(70) ? range(0.0f, 900.0f) : wildf());
    }
    HS->count = chance(10) ? irange(0, 16) : irange(1, 8);
    HS->focus = irange(0, 7); HS->player = chance(70) ? HS->focus : irange(0, 7);
    HS->camera = chance(40) ? 0 : chance(20) ? 0xc : irange(1, 11);
    HS->race_state = chance(30) ? irange(-2, 14) : irange(0, 12);
    { static const int32_t types[] = {0, 0, 0, 3, 5, 1, 2}; HS->game_type = types[rnd() % 7]; }
    *(int32_t*)(g_gameopts + 0x18) = HS->game_type;
    HS->ai_count = irange(0, 7);
    HS->numlaps = irange(0, 9);
    HS->finished = rnd();
    HS->lost = (uint8_t)(chance(20) ? 1 : 0);
    HS->phys_time = fbits_(chance(80) ? range(0.0f, 600.0f) : wildf());
    HS->carfile = irange(-1, 7);
    HS->mr_mask = rnd();
    HS->opt_seed = rnd();
    // what the dash last drew: often the car's own (unchanged), else anything
    const CarInfo* fri = &((CarMgrInfo*)(uintptr_t)(S_CARMGR + 0x170u * (uint32_t)HS->focus))->info;
    RH_G8(S_REDRAW) = (uint8_t)(chance(20) ? 1 : 0);
    RH_G32(0x004e3bb0) = chance(80) ? HS->focus : irange(-1, 8);
    RH_G32(0x004e3bb4) = chance(60) ? (fri->place > 1 ? fri->place : 1) : irange(-1, 9);
    RH_G32(0x004e3bb8) = chance(60) ? (fri->lap > 1 ? fri->lap : 1) : irange(-1, 9);
    RH_GU32(0x004e3bc0) = chance(60) ? fri->best_b : fbits_(wildf());
    RH_GU32(0x004e3bc4) = chance(60) ? fri->last_b : fbits_(wildf());
    RH_G32(0x004e3bd4) = irange(-1, 3);
    RH_G32(S_DIRTY) = chance(20) ? 0xffff : (int32_t)(rnd() & 0x1ff);
    // guards: mostly built
    RH_G8(S_DASH_ONCE) = (uint8_t)(chance(85) ? 0xff : rnd());
    RH_G8(S_DASH_ONCE2) = (uint8_t)(chance(85) ? 1 : rnd());
    RH_G8(S_DRAW_ONCE) = (uint8_t)(chance(80) ? 0xff : chance(50) ? 0xf7 : rnd());
    RH_G8(S_DRAW_ONCE2) = (uint8_t)(chance(85) ? 3 : rnd());
    for (uint32_t g : {0x00505a80u, 0x00505adcu, 0x00505a50u, 0x00505accu, 0x00505a0cu, 0x00505a44u}) RH_G8(g) = (uint8_t)(chance(80) ? 1 : 0);
    // the Xlators (keys: the $E set them up; mostly fresh)
    for (uint32_t xl : {0x005042f8u, 0x00504230u, 0x005041d8u, 0x00504130u, 0x00504188u, 0x005040e8u, 0x00504100u, 0x005041c0u,
                        0x005041b0u, 0x005042b8u, 0x00504280u, 0x00504208u, 0x005042e0u, 0x00504220u, 0x00504168u, 0x005040c8u,
                        0x00504290u, 0x005040d8u, 0x005042a8u, 0x00504110u, 0x00505a98u, 0x00505a58u, 0x00505ad0u, 0x00505a88u,
                        0x00505aa8u, 0x00505a70u}) {
        if (!UI_GU32(xl) || !readable((void*)(uintptr_t)UI_GU32(xl), 1)) UI_GU32(xl) = U32(W.texts + 0x220);
        const bool fresh = chance(70);
        UI_GU32(xl + 4) = fresh ? xl_text(UI_GU32(xl)) : 0;
        UI_GU32(xl + 8) = fresh ? UI_GU32(S_XLATOR_COOKIE) : ~UI_GU32(S_XLATOR_COOKIE);
    }
    // the gear names, once built
    if (RH_G8(S_DRAW_ONCE) & 8) {
        RH_GU32(S_GEAR_NAMES) = U32(W.texts + 0x230);
        RH_GU32(S_GEAR_NAMES + 4) = U32(W.texts + 0x240);
        for (uint32_t k = 0; k < 7; k++) RH_GU32(S_GEAR_NAMES + 8 + 4 * k) = 0x004e4084u + 4 * k;
    }
    // the banners
    for (int k = 0; k < 6; k++) {
        RH_G8(S_SHOW + k) = (uint8_t)(chance(25) ? 1 : 0);
        RH_G8(S_SHOW_NEXT + k) = (uint8_t)(chance(20) ? 1 : 0);
    }
    RH_G8(S_MAIN_DONE) = (uint8_t)(chance(15) ? 1 : 0);
    RH_G8(S_PAUSED) = (uint8_t)(chance(25) ? 1 : 0);
    RH_G8(0x004e53f8) = (uint8_t)(chance(20) ? 1 : 0);          // GhostCarIncognito
    RH_GU32(0x004fb454) = chance(20) ? 1u : 0u;                 // MultiEnabled
    RH_GU32(S_START_SEEN) = chance(40) ? 0u : fbits_(chance(80) ? range(0.0f, 600.0f) : wildf());
    RH_G32(S_CAMERA_STEP) = irange(0, 2);
    RH_GP(S_ST_AWARD) = chance(90) ? (void*)fake_for("award.stp", 'STMP') : 0;
    // the escape menu
    for (int k = 0; k < 2; k++) {
        EscapeMenu* m = &W.menus[k];
        m->count = chance(90) ? irange(1, 4) : irange(-1, 4);
        for (int i = 0; i < 4; i++) {
            m->items[i].text = W.texts + 0x220 + 0x10 * (i & 3);
            m->items[i].id = chance(20) ? (int32_t)0xfff5d3d6 : irange(0, 9);
        }
        m->escape_id = chance(30) ? (int32_t)0xfff5d3d6 : irange(0, 9);
        m->begin = chance(50) ? &esc_begin : 0;
        m->end = chance(50) ? &esc_end : 0;
        m->chosen = chance(50) ? &esc_chosen : 0;
    }
    RH_GP(S_ESC_MENU) = chance(25) ? 0 : (void*)&W.menus[rnd() & 1];
    {
        const EscapeMenu* m = UI_GP(const EscapeMenu, S_ESC_MENU);
        RH_G32(S_ESC_SEL) = m && m->count > 0 && chance(90) ? irange(0, m->count - 1) : irange(-1, 5);
    }
    RH_G32(S_ESC_SINGLE) = chance(90) ? 3 : irange(-1, 20);
    // hack.obj, gxdash.obj
    RH_G8(S_HACK_ON) = (uint8_t)(chance(50) ? 1 : 0);
    RH_G8(S_HACK_SAVED) = (uint8_t)rnd();
    for (uint32_t a : {S_HACK_STATUS, S_HACK_HORN, S_HACK_NO_WALLS, S_HACK_PAVE}) RH_G8(a) = (uint8_t)(chance(20) ? rnd() : rnd() & 1);
    for (uint32_t a : {S_HACK_THROTTLE, S_HACK_GRIP, S_HACK_GRAVITY}) RH_GF(a) = anyf();
    RH_G32(S_HACK_CAR) = irange(-1, 7);
    RH_GF(S_LOD) = chance(60) ? range(0.0f, 21.0f) : anyf();
    // splash.obj
    HS->time = (int32_t)(chance(10) ? rnd() & 0x7fffffff : rnd() & 0x7fffff);
    HS->tstep = chance(50) ? 16 : irange(1, 3000);
    HS->frames = 0;
    HS->frame_limit = 300;
    RH_G32(S_SPLASH_UNTIL) = HS->time + irange(-2000, 3000);
    HS->grab_fail_at = chance(20) ? irange(1, 6) : -1;
    HS->nkeys = HS->key_pos = HS->nmouse = HS->mouse_pos = HS->forced_keys = HS->forced_mouse = 0;
    if (chance(60)) { HS->nkeys = irange(0, 4); for (int i = 0; i < HS->nkeys; i++) HS->keys[i] = irange(0, 0x130); }
    if (chance(60)) { HS->nmouse = irange(0, 6); for (int i = 0; i < HS->nmouse; i++) HS->mouse[i] = irange(0, 6); }
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
        {0x004e3d80, 0x004e4140}, {0x004e5230, 0x004e5400},     // dash / escape, splash, hack, gxdash strings
        {0x004eb108, 0x004eb10c},                               // Xlator::g_cookie
        {0x004efbf8, 0x004efbfc},                               // the current canvas
        {0x005024b8, 0x005024bc},                               // __adjust_fdiv
        {0x005228d4, 0x005228d8}, {0x005228f4, 0x005228f8},
        {0x00504250, 0x00504274},                               // the dash's canvas
        {0x00504140, 0x00504164},                               // and its background
    };
    for (auto& k : keep) memcpy(DATA + (k[0] - 0x004e1000), keepfrom.data + (k[0] - 0x004e1000), k[1] - k[0]);
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static uint32_t canvas_arg() { return chance(80) ? U32(W.canvas) : (uint32_t)S_CANVAS; }
static const char* text_arg() { return chance(15) ? (const char*)0 : W.names[rnd() & 7]; }
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const char* nm = f.name;
    if (nm[0] == '$') return true;
#define IS(n) (!strcmp(nm, n))
    if (IS("TheRealDashboardDraw")) { w[0] = canvas_arg(); w[1] = rnd() & (chance(50) ? 1 : 0xff); }
    else if (IS("DashboardDraw") || IS("DashboardDrawDashNone") || IS("copy_rects") || IS("EscapeMenuDraw") || IS("GXDashboardDraw")) w[0] = canvas_arg();
    else if (IS("clear_rect")) { w[0] = chance(80) ? 1u << irange(0, 8) : rnd(); if (!w[0]) w[0] = 0x10000; }
    else if (IS("draw_analog_dial")) {
        w[0] = (uint32_t)irange(-100, 900); w[1] = (uint32_t)irange(-100, 700); w[2] = (uint32_t)irange(-60, 60);
        w[3] = fbits_(chance(70) ? range(0.0f, 9000.0f) : anyf());
        w[4] = chance(70) ? 0xbe8efa35u : fbits_(anyf());
        w[5] = chance(70) ? 0x3a2ddc49u : fbits_(anyf());
    } else if (IS("EscapeMenuKey") || IS("GXDashboardKey")) {
        static const uint16_t keys[] = {0x1b, 0xd, 0x20, 0x126, 0x128, '\'', '[', ']', 'f', 'l', 'm', 'a', 0x27, 0x6d, 0x6e, 0x26, 0x127};
        w[0] = chance(90) ? keys[rnd() % (sizeof keys / sizeof keys[0])] : rnd() & 0xffff;
    } else if (IS("EscapeMenuDo")) w[0] = U32(&W.menus[rnd() & 1]);
    else if (IS("do_callback")) {
        const EscapeMenu* m = UI_GP(const EscapeMenu, S_ESC_MENU);
        if (!m) return false;                                // the original reads through the null menu
        w[0] = chance(30) ? 0xfff5d3d6u : (uint32_t)irange(-1, 9);
    } else if (IS("generic_splash")) { w[0] = chance(50) ? 0 : U32(W.texts + 0x260); w[1] = U32(text_arg()); w[2] = rnd() & (chance(50) ? 1 : 0xff); }
    else if (IS("SplashGeneric") || IS("SplashButDontGrabScreen")) w[0] = U32(text_arg());
    else if (IS("wait_key_timeout")) { w[0] = (uint32_t)irange(-2, 12); w[1] = rnd() & (chance(50) ? 1 : 0xff); }
    else if (IS("SplashGenericStamp")) { w[0] = U32(W.texts + 0x260); w[1] = rnd() & 1; w[2] = (uint32_t)irange(-1, 8); }
    else if (IS("SplashMustSeeStamp")) { w[0] = U32(W.texts + 0x260); w[1] = (uint32_t)irange(-1, 3); }
    else if (IS("HackBegin")) w[0] = rnd() & (chance(50) ? 1 : 0xff);
    else if (IS("flip_option")) w[0] = chance(50) ? 1u : 0x40u;
    else if (IS("EscapeMenuEnd") || IS("EscapeMenuBegin") || IS("DashboardBegin")) {}
#undef IS
    return true;
}

// ---- main -----------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e) -> LONG {
        printf("the test itself crashed: %08lx at %08lx (esp %08lx)\n", e->ExceptionRecord->ExceptionCode,
               (unsigned long)e->ContextRecord->Eip, (unsigned long)e->ContextRecord->Esp);
        const uint32_t* sp = (const uint32_t*)(uintptr_t)e->ContextRecord->Esp;
        for (int i = 0; i < 24; i++) printf(" %08x", sp[i]);
        printf("\n esi %08lx edi %08lx ecx %08lx\n", (unsigned long)e->ContextRecord->Esi, (unsigned long)e->ContextRecord->Edi,
               (unsigned long)e->ContextRecord->Ecx);
        fflush(stdout);
        ExitProcess(5);
    });
    const int rounds = argc > 1 ? atoi(argv[1]) : 24;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    const char* only = getenv("VP_ONLY");
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_root_hud.cpp");
    if (s) *s = 0;
    strcat(exe, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
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
        {F_SingleEnter, (void*)&stub_SingleEnter}, {F_SingleLeave, (void*)&stub_SingleLeave},
        {F_atexit, (void*)&stub_atexit}, {F_PTimeNow, (void*)&stub_PTimeNow}, {F_MouseGetEvent, (void*)&stub_MouseGetEvent},
        {F_KeyHit, (void*)&stub_KeyHit}, {F_KeyGet, (void*)&stub_KeyGet}, {F_MouseClear, (void*)&stub_MouseClear},
        {F_KeyClear, (void*)&stub_KeyClear}, {F_Win32Idle, (void*)&stub_Win32Idle}, {F_TaskSleep, (void*)&stub_TaskSleep},
        {F_gxGetStamp, (void*)&stub_gxGetStamp}, {F_gxForgetStamp, (void*)&stub_gxForgetStamp},
        {F_gxStampWidth, (void*)&stub_gxStampWidth}, {F_gxStampHeight, (void*)&stub_gxStampHeight},
        {F_gxDrawStamp, (void*)&stub_gxDrawStamp}, {F_gxFontGet, (void*)&stub_gxFontGet}, {F_gxFontForget, (void*)&stub_gxFontForget},
        {F_gxFontHeight, (void*)&stub_gxFontHeight}, {F_gxFontStringWidth, (void*)&stub_gxFontStringWidth},
        {F_gxFontPrintf, (void*)&stub_gxFontPrintf}, {F_gxPaletteCreate, (void*)&stub_gxPaletteCreate},
        {F_gxPaletteDestroy, (void*)&stub_gxPaletteDestroy}, {F_gxPaletteMakeGradient, (void*)&stub_gxPaletteMakeGradient},
        {F_gxSetCanvas, (void*)&stub_gxSetCanvas}, {F_gxClear, (void*)&stub_gxClear}, {F_gxRect, (void*)&stub_gxRect},
        {F_gxLine, (void*)&stub_gxLine}, {F_gxTriangle, (void*)&stub_gxTriangle}, {F_gxPaste, (void*)&stub_gxPaste},
        {F_gxText, (void*)&stub_gxText}, {F_gxSetClip, (void*)&stub_gxSetClip}, {F_gxRestoreClip, (void*)&stub_gxRestoreClip},
        {F_gxAllocCanvas, (void*)&stub_gxAllocCanvas}, {F_gxFreeCanvas, (void*)&stub_gxFreeCanvas},
        {F_gxGrabScreen, (void*)&stub_gxGrabScreen}, {F_gxReleaseScreen, (void*)&stub_gxReleaseScreen}, {F_gxFlip, (void*)&stub_gxFlip},
        {F_mrBeginAlphaRects, (void*)&stub_mrBeginAlphaRects}, {F_mrEndAlphaRects, (void*)&stub_mrEndAlphaRects},
        {F_mrDrawAlphaRect, (void*)&stub_mrDrawAlphaRect}, {F_mrIsEnabled, (void*)&stub_mrIsEnabled},
        {F_mrEnable, (void*)&stub_mrEnable}, {F_mrDisable, (void*)&stub_mrDisable},
        {F_UIStyleDraw, (void*)&stub_UIStyleDraw}, {F_UIStyleWidth, (void*)&stub_UIStyleWidth},
        {F_UIAddDynamicStyle, (void*)&stub_UIAddDynamicStyle}, {F_UIRemoveStyle, (void*)&stub_UIRemoveStyle},
        {F_Xlator_xlate, (void*)&stub_xlate}, {F_sprintf, (void*)&stub_sprintf},
        {F_LocaleConvertNumeric, (void*)&stub_LocaleConvertNumeric}, {F_PhysicsTimeString, (void*)&stub_PhysicsTimeString},
        {F_TelemetrySetAddMeter, (void*)&stub_TelemetrySetAddMeter}, {F_VidWasLost, (void*)&stub_VidWasLost},
        {F_WorldGetFocusCar, (void*)&stub_WorldGetFocusCar}, {F_WorldGetPlayerCar, (void*)&stub_WorldGetPlayerCar},
        {F_WorldGetCameraType, (void*)&stub_WorldGetCameraType}, {F_WorldGetRaceState, (void*)&stub_WorldGetRaceState},
        {F_WorldGameOptions, (void*)&stub_WorldGameOptions}, {F_CarMgrCount, (void*)&stub_CarMgrCount},
        {F_CarMgrGetInfo, (void*)&stub_CarMgrGetInfo}, {F_RecordGetRaceTime, (void*)&stub_RecordGetRaceTime},
        {F_PhysicsGetTime, (void*)&stub_PhysicsGetTime}, {F_AICarCount, (void*)&stub_AICarCount},
        {F_GetCarFileNumber, (void*)&stub_GetCarFileNumber}, {F_OptionsGetF, (void*)&stub_OptionsGetF},
        {F_OptionsGetI, (void*)&stub_OptionsGetI}, {F_OptionsGetB, (void*)&stub_OptionsGetB}, {F_OptionsSetI, (void*)&stub_OptionsSetI},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    build_world();
    mem_save(g_pristine);
    int n_e = 0;
    for (int i = 0; i < g_nfns; i++) n_e += g_fns[i].name[0] == '$';
    printf("world: %d functions listed (%d static initialisers run at setup), the dash begun\n", g_nfns, n_e);

    int dup = 0;
    for (int i = 0; i < g_nfns; i++)
        for (int j = i + 1; j < g_nfns; j++)
            if (g_fns[i].v10 == g_fns[j].v10 || !strcmp(g_fns[i].name, g_fns[j].name)) {
                printf("  listed twice: %08x %s / %08x %s\n", g_fns[i].v10, g_fns[i].name, g_fns[j].v10, g_fns[j].name);
                dup++;
            }

    static Footprint fp;
    long long checks = 0, poisoned_checks = 0, log_words = 0, both_fault = 0;
    int differ = 0, fp_bad = 0, faults = 0, replay_n = 0, changed_fns = 0, still_fns = 0, bad_fns = 0, skipped = 0, fp_checked = 0;
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0, fn_fp = 0;
        bool replay_any = false;
        int nr = rounds;
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            if (poisoned) poison(g_pristine);
            randomize_world();
            uint32_t words[72];
            if (!make_args(f, words)) { skipped++; continue; }
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            if (fp.replay_only) replay_any = true;
            else if (rd == 0 && f.name[0] != '$') nr = rounds * 10;
            mem_save(g_snap);
            const Result ro = run(f, false, words);
            fn_changed |= mem_diff(g_snap) != 0;
            if (!fp.replay_only && !ro.fault) {
                fn_fp++;
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
            mem_load(g_snap);
            const Result rn = run(f, true, words);
            checks++;
            fn_checks++;
            poisoned_checks += poisoned;
            log_words += g_log_orig.n;
            if (ro.fault || rn.fault) {
                faults++;
                fn_faults++;
                if (trace && fn_faults <= 3) {
                    printf("    round %d (%s) faulted: %08x at %08x / %08x at %08x; the original's last logged words:", rd,
                           poisoned ? "poisoned" : "as loaded", ro.code, ro.eip, rn.code, rn.eip);
                    const uint32_t n = g_log_orig.n < LOG_MAX ? g_log_orig.n : LOG_MAX;
                    for (uint32_t k = n > 12 ? n - 12 : 0; k < n; k++) printf(" %08x", g_log_orig.w[k]);
                    printf("\n");
                }
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
                        for (uint32_t k = i >= 8 ? i - 8 : 0; k < i + 3 && k < g_log.n; k++) printf(" %08x/%08x", g_log_orig.w[k], g_log.w[k]);
                        printf(")\n");
                        break;
                    }
                if (getenv("VP_DUMP")) {
                    const uint32_t n = g_log.n > g_log_orig.n ? g_log.n : g_log_orig.n;
                    for (uint32_t i = 0; i < n && i < LOG_MAX; i++) {
                        const uint32_t a = i < g_log_orig.n ? g_log_orig.w[i] : 0, b = i < g_log.n ? g_log.w[i] : 0;
                        char c[5];
                        for (int k = 0; k < 4; k++) { const char ch = (char)(a >> (24 - 8 * k)); c[k] = ch >= 32 && ch < 127 ? ch : '.'; }
                        c[4] = 0;
                        printf("      %4u %08x %08x %s%s\n", i, a, b, c, a != b ? "  <<" : "");
                    }
                }
                differ++;
                fn_bad = true;
            }
        }
        replay_n += replay_any && !fn_fp;
        fp_checked += fn_fp > 0;
        bad_fns += fn_bad;
        (fn_changed ? changed_fns : still_fns)++;
        if (trace)
            printf("%08x %-40s %s%s (%d checks, %d footprint-checked, %d faulted)\n", f.v10, f.name, fn_bad ? "BAD" : "ok",
                   fn_changed ? "" : " (never changed memory)", fn_checks, fn_fp, fn_faults);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    printf("%d functions (%d static initialisers), %d listed twice; %d footprint-checked in some round, %d replay_only in every round; "
           "%lld checks (%lld on poisoned .data), %lld logged words compared: %d differ, %d footprint violations, %d checks faulted "
           "(%lld in both, the same way), %d skipped; %d functions bad\n",
           g_nfns, n_e, dup, fp_checked, replay_n, checks, poisoned_checks, log_words, differ, fp_bad, faults, both_fault, skipped, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    return differ || fp_bad || dup ? 1 : 0;
}
