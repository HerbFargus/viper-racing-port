// world_root_race.cpp -- M3 UI stage, step U3 (group B): every rewrite of hook/root_main.cpp (main.obj, version.obj, WinMain)
// and hook/root_race.cpp (race.obj, prerace.obj, postrace.obj) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_root_race.cpp
//        /Fo<dir>\ /Fe<dir>\world_root_race.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_root_race.exe [rounds] [seed]          (VP_TRACE=1: one line per function; VP_ONLY=text: those only;
//          VP_DEBUG_WORLD=1: the world's objects and every fault's registers; VP_DEBUG_LOG=1: each round's first logged words)
//     (and with /DVP_ROOT_FIXES: the fix build, below)
//
// Loads out\race_v10.exe at 0x400000 the way test/world_menu_view.cpp does (a child process with the range reserved) and
// includes the two rewrite files with PORT_FN redefined to list each function (its v1.0 address, the rewrite, its calling
// convention -- __cdecl, __thiscall as __fastcall, WinMain's __stdcall --, stack arguments and return, its footprint).
// VP_FAITHFUL: the rewrites exactly as the originals.
//
// The world. Once: the $E static initialisers of libraries ui, menu and root (their Xlators, colours, tables), the game's
// UIBegin (the styles); a locale; a fake StringTable for tracks.tab, a car list, a CarMgr (infos), race and lap records, a
// Deity (its vtable's three getters the code calls: stubs); a World (8 cars); then the objects by their own original
// constructors where they have one -- a CarPosition, four more for a StartingGridView, a LapsTable -- and written as the
// originals' inlined constructors write them otherwise (the StartingGridView, a TrackPrizeInfo, a PostRaceTable with two
// styles from UIAddDynamicStyle); all put in a full-screen window by the game's WidgetCreateWindow and _UIAddItems
// (CustomWidgets: adding one runs its control's Added), then their Create. This is the pristine state.
//
// Then for EVERY function, `rounds` times (ten times that for one that doesn't run a dialog or a loop): pristine restored;
// half the rounds POISONED (.data/.bss random but for the strings and tables the code reads and what the ui library
// follows); the world's state random (the World's cars, types and options; the objects' fields -- times NaN / infinite /
// tiny, grid slots and indexes out of range, pages, laps --; the records, the car list; the statics the code reads: the
// camera, the dashboard, the application mode, the pause / restart / race-over flags, the blimp's vectors and frame, the
// escape menus built or not, every function-local Xlator built or not, fresh or stale); arguments made for the function
// (matrices with every aliasing, NaN / infinite angles, command lines, frames); for the loops and dialogs a random input
// script and random answers from the stubs, ended by a watchdog (the game state's "over", the main menu's "quit", the
// dialogs' frame limit). The stack filled with one pattern after the FPU is set; the ORIGINAL runs from a raw call thunk;
// its results kept, the snapshot restored, the REWRITE runs. Compared: .data/.bss/.idata and the 1 MB arena (a pointer
// into this thread's stack in both counts as equal), the return value (st0 where there's one), the x87 depth, bytes
// popped, ebx / esi / edi / ebp, faults, and every stub's call log. Every byte the original changed must lie in the
// rewrite's footprint (unless replay_only); a pure one changes nothing but its output. The x87 at 24 or 53 bits.
//
// Stubbed (logged; their state in the arena so both passes see the same): world_menu_view.cpp's set (memory, logs, input and
// time, the 2D calls -- gxFontPrintf logs its format's arguments --, Xlator::xlate, sprintf, files) and every callee outside
// the widget toolkit that starts, runs or ends a part of the game (graphics, sound, options, resources, the kernel, the
// physics' run state, the world, the dashboard, the escape menu, the countdown, the multiplayer, the menus, the replay,
// the garage, the board, the credits, the intro), the records and CarMgr, ScanDown, the StringTable, the clipboard's and
// the window's imports (their IAT slots), the 3D renderer, the Deity's getters, the dashboard table's functions. The game's
// own code everywhere else: the widget toolkit (UIDoDialog, _UIAddItems, the groups, UIStyleDraw / WordWrap,
// UIAddDynamicStyle), UIDialogItem's, Xlator's, World's, BoardCustomText's constructors, gxSetClip / gxRestoreClip, the
// CRT (sscanf, strstr, strncmp, qsort), Base64ToMem / MemToBase64, blimp_to_tv, Win32GetWindow, and every function of this
// group (each rewrite is checked against its original with the same callees).
//
// Built with /DVP_ROOT_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Race front
// end"). Every function is still compared as above, with the rounds that reach a fixed case kept out of the comparison
// and counted (per function, listed), the rewrite still run on each and required to return cleanly: GetEventString's where
// the original's sprintf printed past one of its three 0x20-byte texts (sprintf's stub knows them: a huge locale factor's
// number with the longer translation does that), CarPosition::CarPosition's with a car name of 12 or more characters (the
// fixed remap names stop at their 16 bytes, where the original's ran on over fields it then wrote again), and
// AppProcessArgs' where only the original faults (a random text with a -d and no space after it: the original writes to
// address 0). Then directed_fix_tests: for each fix, the bad case run on the rewrite alone -- no fault, the bytes popped
// and ebx / esi / edi / ebp kept, nothing written outside what it may write (the memory around it compared), and the
// result the fix promises -- and its boundary case (the longest input that fits) on both, compared bit for bit; and
// setup_blimp_jump's word scan against the game's own sscanf (random command lines, every white-space byte). Without it
// (VP_FAITHFUL) every rewrite must match its original bit for bit.
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
#include <string>
#include <vector>

#ifndef VP_ROOT_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#define ROOT_FIXES 0
#else
#define ROOT_FIXES 1
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
template <typename R, typename... A> struct Sig<R(__stdcall*)(A...)> {
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
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                                 \
    static EntReg VP_CAT(reg_, NEW)({V10, NAME, (void*)&NEW, Sig<decltype(&NEW)>::fast, Sig<decltype(&NEW)>::nstack,   \
                                     Sig<decltype(&NEW)>::ret, Sig<decltype(&NEW)>::retf, &Sig<decltype(&NEW)>::fp<&FP>});

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { printf("footprint: object() not expected (%s)\n", what); add(obj, 4, what); }
void Footprint::stack_ptr(void* p, const char* what) { add(p, 4, what); }

#include "../hook/root_main.cpp"
#include "../hook/root_race.cpp"

using namespace uit;
using namespace rootb;

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
    uint32_t rs;                                  // the stubs' random answers
    uint32_t scan_mask[8];                        // ScanDown's keys held (256 bits)
    int32_t menu_calls, menu_end;                 // MenuDoRaceMenu: "quit" (8) from menu_end on
    int32_t state_calls, state_end;               // GetGameState: "over" (3) from state_end on
    int32_t player, car_count, track_no, tab_rows, deity_laps;
    int32_t laps[16];                             // RecordGetLapResult: each car's laps
    uint32_t rec_mask;                            // RecordGetRaceResult: the cars with a result
    int32_t place[16];                            // Deity::GetCarByPlace
    int32_t find_n, find_pos;
    char find_names[24][40];
    uint32_t opt_mask;
    int32_t opt_val[8];
    uint32_t cl_len;                              // CenterLine's length (+0x5c), bits
    int32_t userdir_len;
    int32_t board_next;
};
#define HS ((HState*)g_arena)
enum : uint32_t { A_HEAP = 0x60000 };
static uint32_t hs_rand() {
    uint32_t x = HS->rs;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    HS->rs = x;
    return (x * 2654435761u) ^ (x >> 16);                // (mixed: the stream's low bits alone are weak)
}
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
static bool g_overran;                                 // sprintf printed past one of GetEventString's texts
static const char* g_fx_xl_name;                      // a track's friendly name (a "Tracks:Name" key's text)
static const char* g_fx_xl_text;                      // a track's text (a "Tracks:Text" key's)
static const char* const* g_fx_find;                  // FileFindFirst / Next's names (g_fx_find_n of them)
static int g_fx_find_n;
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
// (with its format's arguments: a string's text, a number's dwords)
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
    // (a key on this thread's stack -- a poisoned one -- differs between the passes: not read)
    const uint32_t h = (on_stack(key) ? 0x5eedu : hash_str((const char*)(uintptr_t)key)) ^ g_xl_seed;
    return k_xl_text[(h & 0xff) < 0xf0 ? h % 14 : 14 + (h & 1)];
}
static bool key_is(uint32_t key, const char* prefix) {
    const char* k = (const char*)(uintptr_t)key;
    return readable(k, 1) && readable(k, (uint32_t)strlen(prefix)) && !strncmp(k, prefix, strlen(prefix));
}
static void __fastcall stub_xlate(uint32_t* xl, int) {
    L('XLAT'); L(P(xl));
    const char* t = g_fx_xl ? g_fx_xl : xl_text(xl[0]);
    if (g_fx_xl_name && key_is(xl[0], "Tracks:Name")) t = g_fx_xl_name;
    if (g_fx_xl_text && key_is(xl[0], "Tracks:Text")) t = g_fx_xl_text;
    xl[1] = (uint32_t)(uintptr_t)t;
    xl[2] = UI_GU32(S_XLATOR_COOKIE);
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
static int32_t __cdecl stub_FileOpen(const char* name) { L('FOPN'); LS(name); return 0; }
static void __cdecl stub_FileClose(int32_t* fd) { L('FCLS'); L((uint32_t)*fd); *fd = 0; }
static uint8_t __cdecl stub_ClipboardGetText(char* buf, int n) { L('CLIP'); if (n > 0) buf[0] = 0; return 0; }

// ---- stubs: this group's callees (text, records, the world, the game's parts) ----------------------------------------------------
static const char* const k_tracks[] = {"bemidji", "heaven", "uptown", "hastings", "limbo", "dundas", "nfield", "kenyon", "hell"};
static const char* const k_cars[] = {"viper", "cobra", "gts", "rt10", "stingray", "zr1", "288gto", "willys"};
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
        if (*p == 's') LS((const char*)(uintptr_t)w[nw]);
        nw += strchr("eEfgG", *p) ? 2 : 1;
    }
    for (int i = 0; i < nw && i < 16; i++) L(on_stack(w[i]) ? 'STAK' : w[i]);
    const int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    LS(buf);
    const uint32_t b = (uint32_t)(uintptr_t)buf;
    if ((b == S_EVENT_TEXT0 || b == S_EVENT_TEXT1 || b == S_EVENT_TEXT2) && strlen(buf) > 0x1f) g_overran = true;
    return r;
}
static void* __cdecl stub_FindFirst(const char* pat, char* buf, int n) {
    L('FFF '); LS(pat); L((uint32_t)n);
    if (g_fx_find) {
        if (g_fx_find_n <= 0) return (void*)(intptr_t)-1;
        strcpy(buf, g_fx_find[0]);
        HS->find_pos = 1;
        return (void*)(uintptr_t)0x00f1f1f1;
    }
    if (HS->find_n <= 0) return (void*)(intptr_t)-1;
    strcpy(buf, HS->find_names[0]);
    HS->find_pos = 1;
    return (void*)(uintptr_t)0x00f1f1f1;
}
static uint8_t __cdecl stub_FindNext(void* h, char* buf, int n) {
    L('FFN '); L((uint32_t)(uintptr_t)h); L((uint32_t)n);
    if (g_fx_find) {
        if (HS->find_pos >= g_fx_find_n) return 0;
        strcpy(buf, g_fx_find[HS->find_pos++]);
        return 1;
    }
    if (HS->find_pos >= HS->find_n) return 0;
    strcpy(buf, HS->find_names[HS->find_pos++]);
    return 1;
}
static void __cdecl stub_FindClose(void* h) { L('FFC '); L((uint32_t)(uintptr_t)h); }
static void __cdecl stub_OptionsGet(const char* sec, const char* key, int32_t* out) {
    L('OPTG'); LS(sec); LS(key); L(P(out));
    const uint32_t h = hash_str(key) ^ HS->opt_mask;
    if (h & 1) { *out = HS->opt_val[(h >> 1) & 7]; L((uint32_t)*out); }
}
static int __cdecl stub_stricmp(const char* a, const char* b) {
    L('SICM'); LS(a); LS(b);
    if (!readable(a, 1) || !readable(b, 1)) *(volatile int*)0 = 0;
    const int r = _stricmp(a, b);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
static int __cdecl stub_strnicmp(const char* a, const char* b, uint32_t n) {
    L('SNCM'); LS(a); LS(b); L(n);
    if (!readable(a, 1) || !readable(b, 1)) *(volatile int*)0 = 0;
    const int r = _strnicmp(a, b, n);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
// tracks.tab: 16 rows of name, friendly name's key, text's key
static uint8_t g_tab[16];
static char g_tab_str[16][3][40];
static void* __cdecl stub_StringTableGet(const char* name) { L('STGT'); LS(name); return g_tab; }
static int32_t __cdecl stub_StringTableNumRows(const void* t) { L('STNR'); L(P(t)); return HS->tab_rows; }
static const char* __cdecl stub_StringTableGetEntry(const void* t, int32_t row, int32_t col) {
    L('STEN'); L(P(t)); L((uint32_t)row); L((uint32_t)col);
    return g_tab_str[(uint32_t)row & 15][(uint32_t)col % 3];
}
static uint32_t __cdecl stub_MenuDoRaceMenu() {
    L('MDRM');
    const int32_t n = ++HS->menu_calls;
    const uint32_t r = n >= HS->menu_end ? 8u : hs_rand() % 10u;
    L(r);
    return r;
}
static int32_t __cdecl stub_GetGameState() {
    L('GGST');
    const int32_t n = ++HS->state_calls;
    const int32_t r = n >= HS->state_end ? 3 : (hs_rand() % 5 == 0 ? 3 : (int32_t)(hs_rand() % 3));
    L((uint32_t)r);
    return r;
}
static uint8_t __cdecl stub_ScanDown(uint32_t k) { L('SCND'); L(k & 0xff); return (uint8_t)((HS->scan_mask[(k >> 5) & 7] >> (k & 31)) & 1); }
static int32_t __cdecl stub_WorldGetPlayerCar() { L('WGPC'); return HS->player; }
static int32_t __cdecl stub_CarMgrCount() { L('CMCN'); return HS->car_count; }
static uint8_t* g_infos;                          // CarMgrInfo[16], 0x150 each (arena)
static const void* __cdecl stub_CarMgrGetInfo(int32_t i) { L('CMGI'); L((uint32_t)i); return g_infos + ((uint32_t)i & 15) * 0x150u; }
static uint8_t* g_opts;
static const void* __cdecl stub_WorldGameOptions() { L('WGOP'); return g_opts; }
static RbRaceRecord* g_recs;                      // 16 (arena)
static const void* __cdecl stub_RecordGetRaceResult(int32_t i) {
    L('RGRR'); L((uint32_t)i);
    return ((HS->rec_mask >> ((uint32_t)i & 31)) & 1) ? &g_recs[(uint32_t)i & 15] : 0;
}
static const void* __cdecl stub_RecordGetLapResult(int32_t car, int32_t lap) {
    L('RGLR'); L((uint32_t)car); L((uint32_t)lap);
    if (lap >= 1 && lap <= HS->laps[(uint32_t)car & 15]) return &g_recs[((uint32_t)car * 3u + (uint32_t)lap) & 15];
    return 0;
}
static char g_time[16][16];
static const char* __cdecl stub_PhysicsTimeString(uint32_t bits, uint32_t h) {
    L('PTST'); L(bits); L(h & 0xff);
    char* s = g_time[bits & 15];
    sprintf(s, "t%08x", bits);
    return s;
}
static uint8_t __cdecl stub_get_user_directory() {
    L('GUDR');
    char* d = (char*)(uintptr_t)S_W32_USERDIR;
    const int32_t n = HS->userdir_len;
    for (int32_t i = 0; i < n; i++) d[i] = (char)('a' + i % 26);
    d[n > 0 ? n : 0] = 0;
    return (uint8_t)(hs_rand() & 3 ? 1 : 0);
}
// BoardCreateControl: a UICustomControl of the base's vtable but for its deleting destructor (logged)
static uint32_t g_fake_vt[20];
static uint8_t* g_boards;                         // 8 controls of 0x18 (arena)
static void* __fastcall stub_board_sdd(void* self, int, uint32_t flags) { L('BSDD'); L(P(self)); L(flags); return self; }
static void* __cdecl stub_BoardCreateControl(const char* t, int32_t r, int32_t y, uint32_t m) {
    L('BCRC'); LS(t); L((uint32_t)r); L((uint32_t)y); L(m & 0xff);
    uint32_t* c = (uint32_t*)(g_boards + ((uint32_t)HS->board_next++ & 7) * 0x18u);
    memset(c, 0, 0x18);
    c[0] = (uint32_t)(uintptr_t)g_fake_vt;
    return c;
}
static void* __fastcall stub_CenterLine_ctor(void* self, int, const uint8_t* rev) {
    L('CLCT'); L(P(self)); L(P(rev));
    memset(self, 0x5c, 0x70);
    ((uint32_t*)self)[0x5c / 4] = HS->cl_len;
    return self;
}
static void __fastcall stub_CenterLine_dtor(void* self, int) { L('CLDT'); L(P(self)); }
static void __cdecl stub_WorldGetCarTexture(char* out, const char* base, int32_t n) {
    L('WGCT'); L(P(out)); LS(base); L((uint32_t)n);
    if (g_fx_real_tex) sprintf(out, n < 0 ? "~%s.tex" : "%s.tex", base);           // (the fix tests: the game's names)
    else sprintf(out, "%.8s%d", readable(base, 1) ? base : "?", n & 0xff);
}
static int32_t __cdecl stub_mrModelLoadRemap(const char* name, const char* remap, int32_t n) {
    L('MRLR'); LS(name); LS(remap); LS(remap + 0x10); L_block(remap + 0x20, 2); L((uint32_t)n);
    rec(g_rec_remap_load, sizeof g_rec_remap_load, name);                          // (for the fix tests)
    if (readable(remap, 0x28)) memcpy(g_rec_remap, remap, 0x28);
    return 0x300 + (int32_t)(hash_str(name) & 0xff);
}
static gxCanvas* g_shadow;
static gxCanvas* __cdecl stub_gxCanvasGet(const char* name) { L('GCVG'); LS(name); return g_shadow; }
static void __cdecl stub_gxPasteAlpha(const gxCanvas* c, int32_t x, int32_t y) { L('PSTA'); L(P(c)); L((uint32_t)x); L((uint32_t)y); L_canvas(); }
static void __cdecl stub_mrSetCamera(const void* fr) { L('MRSC'); L_block(fr, 12); }
static void __cdecl stub_mrModelDraw(int32_t m, const void* fr) { L('MRDR'); L((uint32_t)m); L_block(fr, 12); }
static void __cdecl stub_LocaleMoney(char* buf, int32_t v, uint32_t b) { L('LMNY'); L(P(buf)); L((uint32_t)v); L(b & 0xff); sprintf(buf, "$%d", v); }
static void __cdecl stub_LocaleFormatShortDate(char* buf, int32_t n, int32_t d, int32_t m, int32_t y) {
    L('DATE'); L(P(buf)); L((uint32_t)n); L((uint32_t)d); L((uint32_t)m); L((uint32_t)y);
    _snprintf(buf, (size_t)(n > 0 ? n : 0), "%d/%d/%d", d, m, y);
    if (n > 0) buf[n - 1] = 0;
}
// the imports (their IAT slots: the loaded image's aren't bound)
static int __stdcall stub_OpenClipboard(void* h) { L('OCLP'); L((uint32_t)(uintptr_t)h); return (hs_rand() & 3) != 0; }
static int __stdcall stub_EmptyClipboard() { L('ECLP'); return (hs_rand() & 3) != 0; }
static void* __stdcall stub_GlobalAlloc(uint32_t fl, uint32_t n) {
    L('GALC'); L(fl); L(n);
    if (!(hs_rand() & 3)) return 0;
    uint32_t a = (HS->heap_next + 7) & ~7u;
    HS->heap_next = a + n;
    return g_arena + a;
}
static void* __stdcall stub_GlobalLock(void* h) { L('GLCK'); L(P(h)); return h; }
static int __stdcall stub_GlobalUnlock(void* h) { L('GULK'); L(P(h)); return 1; }
static void* __stdcall stub_SetClipboardData(uint32_t f, void* h) { L('SCLD'); L(f); L(P(h)); if (readable(h, 0x100)) L_block(h, 0x40); return h; }
static int __stdcall stub_CloseClipboard() { L('CCLP'); return 1; }
static int __stdcall stub_IsWindow(void* h) { L('ISWN'); L((uint32_t)(uintptr_t)h); return (hs_rand() & 1); }
static int __stdcall stub_DestroyWindow(void* h) { L('DSWN'); L((uint32_t)(uintptr_t)h); return 1; }
// the Deity's getters (its vtable's slots 0x20, 0x40, 0x44)
static uint32_t g_deity_vt[24];
static uint8_t* g_deity;
static int32_t __fastcall stub_deity_laps(void* self, int) { L('DGNL'); L(P(self)); return HS->deity_laps; }
static uint8_t __fastcall stub_deity_finished(void* self, int, int32_t car) { L('DCIF'); L(P(self)); L((uint32_t)car); return (uint8_t)(hs_rand() & 1); }
static int32_t __fastcall stub_deity_byplace(void* self, int, int32_t i) { L('DGBP'); L(P(self)); L((uint32_t)i); return HS->place[(uint32_t)i & 15]; }
static void __fastcall stub_deity_other(void* self, int) { L('DOTH'); L(P(self)); }
// the dashboards' functions (main.obj's table), a race's post-race function, UIBegin / UIEnd (after the world is built)
static void __cdecl stub_dash_update() { L('DUPD'); }
static void __cdecl stub_dash_draw3d() { L('DD3D'); }
static void __cdecl stub_dash_draw2d(void* c) { L('DD2D'); L(P(c)); L_canvas(); }
static uint8_t __cdecl stub_dash_key(uint32_t k) { L('DKEY'); L(k & 0xffff); return (uint8_t)((hs_rand() & 3) == 0); }
static void __cdecl stub_post(void* w) { L('POST'); L(P(w)); }
static void __cdecl stub_UIBegin() { L('UIBG'); }
static void __cdecl stub_UIEnd() { L('UIEN'); }

// ---- the generic stubs: logged arguments (d a dword -- a pointer into the stack as one word --, b its low byte, w its low
// word, s a string), a random answer from the arena's stream (v none, b mostly 0, B mostly 1, i 0..3) ------------------------------
#define GEN_STUBS(X)                                                                                                          \
    X(0x00419560, "", 'v') X(0x00419590, "", 'v') X(0x004cc600, "", 'v') X(0x0044deb0, "", 'v') X(0x0044e7e0, "", 'v')     \
    X(0x0044df20, "d", 'v') X(0x004cccd0, "s", 'v') X(0x00470ee0, "", 'v') X(0x0041a7f0, "", 'v') X(0x00471cb0, "", 'B')   \
    X(0x00419a30, "s", 'v') X(0x0040d380, "b", 'v') X(0x004a1670, "", 'v') X(0x004c8600, "", 'v') X(0x00420bd0, "", 'v')   \
    X(0x00412d80, "", 'v') X(0x00420c00, "", 'v') X(0x004a1680, "", 'v') X(0x0040d4a0, "", 'v') X(0x00419bb0, "s", 'v')    \
    X(0x00471d90, "", 'v') X(0x0044e000, "", 'v') X(0x0044e800, "", 'v') X(0x0044dee0, "", 'v') X(0x0041ab00, "", 'v')     \
    X(0x00471330, "", 'v') X(0x0044e1b0, "", 'v') X(0x00411ce0, "s", 'B') X(0x0040a680, "", 'v') X(0x0040a7e0, "", 'v')    \
    X(0x004bc570, "", 'v') X(0x004819a0, "", 'v') X(0x00406bc0, "d", 'v') X(0x004b0a30, "", 'v') X(0x0041f050, "d", 'b')   \
    X(0x00471110, "", 'v') X(0x00471430, "ssd", 'v') X(0x00413770, "", 'v') X(0x0040ac90, "", 'v') X(0x004719a0, "", 'v')  \
    X(0x00461dd0, "d", 'v') X(0x00419b60, "", 'v') X(0x00403460, "", 'v') X(0x0040c520, "", 'v') X(0x00471de0, "", 'v')    \
    X(0x00412cd0, "", 'v') X(0x0042bea0, "d", 'v') X(0x0040cc40, "", 'v') X(0x0042ba20, "", 'v') X(0x0041d340, "", 'b')    \
    X(0x0041cdf0, "d", 'v') X(0x00471dd0, "", 'v') X(0x004a2430, "", 'v') X(0x0042bba0, "", 'v') X(0x00403720, "", 'v')    \
    X(0x0040c540, "", 'v') X(0x00419ba0, "", 'v') X(0x00461f40, "", 'v') X(0x0042a280, "", 'v') X(0x004a2830, "", 'v')     \
    X(0x00413780, "", 'v') X(0x00461ef0, "d", 'v') X(0x004719e0, "", 'v') X(0x0040acb0, "", 'v') X(0x00471df0, "", 'v')    \
    X(0x00413d90, "", 'v') X(0x00413cf0, "b", 'v') X(0x00413db0, "", 'v') X(0x004a2420, "", 'v') X(0x0042bcf0, "", 'v')    \
    X(0x00403840, "", 'v') X(0x0042bc00, "", 'v') X(0x0042bd40, "", 'i') X(0x00462100, "", 'v') X(0x00462200, "b", 'v')    \
    X(0x00471b20, "d", 'v') X(0x0041d190, "", 'v') X(0x0042be10, "", 'v') X(0x00404820, "", 'v') X(0x00404a80, "", 'b')    \
    X(0x0040c560, "", 'b') X(0x00462570, "d", 'v') X(0x0040aca0, "", 'v') X(0x0040c5a0, "d", 'v') X(0x004a2690, "", 'v')   \
    X(0x0040c660, "w", 'v') X(0x004a23c0, "", 'b') X(0x004a23d0, "", 'b') X(0x0042bcc0, "", 'v') X(0x0040c730, "d", 'v')   \
    X(0x00462690, "", 'v') X(0x004626b0, "", 'v') X(0x004626f0, "d", 'v') X(0x0042bb50, "", 'v') X(0x004a2820, "", 'v')    \
    X(0x004a2a80, "", 'b') X(0x0040a840, "d", 'v') X(0x0040a870, "d", 'v') X(0x004123b0, "", 'B') X(0x004123a0, "", 'B')   \
    X(0x00418620, "", 'B') X(0x004a36a0, "w", 'v') X(0x00418710, "", 'v') X(0x00412490, "", 'v') X(0x00418340, "", 'B')    \
    X(0x00418460, "", 'v') X(0x00412420, "", 'v') X(0x0041b270, "d", 'v') X(0x004911a0, "ssds", 'B')                       \
    X(0x004654b0, "dss", 'B') X(0x0040cbc0, "", 'v') X(0x00455950, "d", 'v') X(0x0044fdc0, "d", 'v')                       \
    X(0x0044ecd0, "ddddb", 'v') X(0x0044ede0, "ddd", 'v') X(0x00490740, "sddb", 'v') X(0x00462760, "", 'i')
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
        case 'w': L(a[k] & 0xffff); break;
        case 's': LS((const char*)(uintptr_t)a[k]); break;
        }
    switch (g.ret) {
    case 'b': return (hs_rand() & 3) == 0 ? 1u : 0u;
    case 'B': return (hs_rand() & 7) != 0 ? 1u : 0u;
    case 'i': return hs_rand() & 3;
    }
    return 0;
}
template <int ID> static uint32_t __cdecl gen_stub(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    const uint32_t a[6] = {a0, a1, a2, a3, a4, a5};
    return gen_body(ID, a);
}
static void patch_jmp(uint32_t at, void* to);
template <size_t... I> static void patch_gen(std::index_sequence<I...>) { (patch_jmp(g_gen[I].at, (void*)&gen_stub<(int)I>), ...); }
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
    if (getenv("VP_DEBUG_WORLD"))
        printf("  fault at %08x: address %08x, eax %08x ecx %08x edx %08x esi %08x edi %08x\n", (uint32_t)e->ContextRecord->Eip,
               (uint32_t)e->ExceptionRecord->ExceptionInformation[1], (uint32_t)e->ContextRecord->Eax, (uint32_t)e->ContextRecord->Ecx,
               (uint32_t)e->ContextRecord->Edx, (uint32_t)e->ContextRecord->Esi, (uint32_t)e->ContextRecord->Edi);
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
        if ((!strcmp(c[2], "ui") || !strcmp(c[2], "menu") || !strcmp(c[2], "root")) && c[5][0] == '$' && c[5][1] == 'E')
            g_setup_e.push_back((uint32_t)strtoul(c[0], 0, 16));
    }
    fclose(f);
}

struct World_ {
    gxCanvas* screen;
    gxCanvas* canvas2;
    WidgetWindow* win;
    uint8_t* world;                               // a World (0xcd4)
    int32_t* res;                                 // Results (0x31 dwords)
    uint8_t* locale;
    CarPosition* cp;
    StartingGridView* grid;
    TrackPrizeInfo* tpi;
    LapsTable* lt;
    PostRaceTable* prt;
    uint32_t* times;                              // 16 floats' bits
    int32_t* prizes;                              // 8 x {points, purse}
    char* carlist;                                // 32 x 32
    uint8_t* dash_keys[5];
    float* mblock;                                // 64 floats: the matrices (and their overlaps)
    float* frame;                                 // a Frame (0x30), a CompactFrame (0x18), outputs
    float* cframe;
    float* frame_out;
    float* cframe_out;
    char* texts[8];
    uint8_t* scratch;                             // for constructors (0x200)
    uint8_t* flags;
    char* cmdlines[12];
    char* locs[8];
    std::vector<Widget*> widgets;
};
static World_ W;

static void fill_world_car(uint8_t* e, int32_t type, const char* car, const char* drv, int32_t idx) {
    *(int32_t*)e = type;
    strncpy((char*)e + 4, drv, 12);
    strncpy((char*)e + 0x11, car, 0x22);
    *(int32_t*)(e + 0xc0) = idx;
}
static void build_world() {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    HS->rs = 0x12345;
    HS->tab_rows = 9;
    HS->state_end = 1 << 30;
    HS->menu_end = 1 << 30;
    UI_G32(S_SCREEN_W) = 640;
    UI_G32(S_SCREEN_H) = 480;
    for (int r = 0; r < 16; r++) {
        strcpy(g_tab_str[r][0], k_tracks[r % 9]);
        sprintf(g_tab_str[r][1], "Tracks:Name%d", r);
        sprintf(g_tab_str[r][2], "Tracks:Text%d", r);
    }
    g_infos = (uint8_t*)wv(16 * 0x150);
    for (int i = 0; i < 16; i++) { sprintf((char*)g_infos + i * 0x150 + 5, "Info car %d", i); *(int32_t*)(g_infos + i * 0x150 + 0x140) = 100 + i; }
    g_opts = (uint8_t*)wv(0x28);
    g_recs = (RbRaceRecord*)wv(16 * sizeof(RbRaceRecord));
    g_boards = (uint8_t*)wv(8 * 0x18);
    g_deity = (uint8_t*)wv(0x800);
    for (int i = 0; i < 24; i++) g_deity_vt[i] = U(&stub_deity_other);
    g_deity_vt[8] = U(&stub_deity_laps); g_deity_vt[16] = U(&stub_deity_finished); g_deity_vt[17] = U(&stub_deity_byplace);
    *(uint32_t*)g_deity = U(g_deity_vt);
    memcpy(g_fake_vt, (const void*)(uintptr_t)VT_UICustomControl, sizeof g_fake_vt);
    g_fake_vt[0] = U(&stub_board_sdd);
    for (uint32_t a : g_setup_e) call_orig(a, 0, {});
    call_orig(0x004779a0, 0, {});                      // UIBegin
    W.screen = (gxCanvas*)wv(sizeof(gxCanvas));
    W.screen->format = 4; W.screen->pixels = (uint8_t*)0x7b000000; W.screen->w = 640; W.screen->h = 480; W.screen->pitch = 1280;
    W.screen->cx1 = 640; W.screen->cy1 = 480;
    W.canvas2 = (gxCanvas*)wv(sizeof(gxCanvas));
    memcpy((void*)W.canvas2, (const void*)W.screen, sizeof(gxCanvas));
    W.canvas2->w = 320; W.canvas2->h = 200; W.canvas2->cx0 = 10; W.canvas2->cy0 = 5; W.canvas2->cx1 = 300; W.canvas2->cy1 = 190;
    g_shadow = (gxCanvas*)wv(sizeof(gxCanvas));
    memcpy((void*)g_shadow, (const void*)W.screen, sizeof(gxCanvas));
    g_shadow->w = 90; g_shadow->h = 31;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    W.locale = (uint8_t*)wv(0x40);
    UI_GP(uint8_t, S_LOCALE) = W.locale;
    *(float*)(W.locale + 0x14) = 1.0f; *(float*)(W.locale + 0x1c) = 1.0f;
    W.carlist = (char*)wv(32 * 32);
    for (int i = 0; i < 8; i++) strcpy(W.carlist + 32 * i, k_cars[i]);
    UI_GP(char, S_CARLIST) = W.carlist;
    UI_G32(S_CARLIST_N) = 8;
    UI_GP(void, S_TRACK_TAB) = g_tab;
    UI_GP(void, S_DEITY) = g_deity;
    W.world = (uint8_t*)wv(0xcd4);
    *(int32_t*)W.world = 3; *(int32_t*)(W.world + 4) = 0xcd4;
    strcpy((char*)W.world + 8, "uptown");
    *(int32_t*)(W.world + 0xca8) = 8;
    for (int i = 0; i < 8; i++) {
        char d[16];
        sprintf(d, "Driver %d", i);
        fill_world_car(W.world + 0x28 + i * 0xc8, i == 0 ? 0 : 1, k_cars[i], d, i == 0 ? -1 : i);
    }
    W.res = (int32_t*)wv(0x31 * 4);
    W.times = (uint32_t*)wv(16 * 4);
    W.prizes = (int32_t*)wv(16 * 4);
    for (int i = 0; i < 16; i++) W.prizes[i] = 10 + i;
    for (int d = 0; d < 5; d++) { W.dash_keys[d] = (uint8_t*)wv(12); memset(W.dash_keys[d], 0xff, 12); }
    W.mblock = (float*)wv(64 * 4);
    W.frame = (float*)wv(0x40); W.cframe = (float*)wv(0x20); W.frame_out = (float*)wv(0x40); W.cframe_out = (float*)wv(0x20);
    for (int i = 0; i < 8; i++) W.texts[i] = (char*)wv(0x40);
    W.scratch = (uint8_t*)wv(0x200);
    W.flags = (uint8_t*)wv(16);
    static const char* const cmds[12] = {"", "-dedicated", "-dedicated:2301", "-u b", "-U D -z", "-ur", "-grid -tri",
                                         "/1 -z", "-location ( uptown AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA )", "-x -uq", "-dfoo bar",
                                         "-location ( hell )"};
    for (int i = 0; i < 12; i++) W.cmdlines[i] = wstr(cmds[i]);
    static const char* const locs[8] = {"( uptown AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA )", "( kenyon AACAgD8AAIA/AACAPwAAAD8AAAA/AAAAPw== )",
                                        "( limbo !!!! )", "( hell )", "junk", "", "( bemidji zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz )",
                                        "(heaven AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA)"};
    for (int i = 0; i < 8; i++) W.locs[i] = wstr(locs[i]);
    // the objects: CarPositions by their constructor, the others as the originals build them
    W.cp = (CarPosition*)wv(sizeof(CarPosition));
    call_orig(0x00410360, 1, {U(W.cp), 0, U(W.world + 0x28 + 0x11), U(W.world + 0x28 + 4), (uint32_t)-1, 0x42f00000u, 2});
    W.grid = (StartingGridView*)wv(0x60);
    W.grid->vtbl = (const void*)(uintptr_t)VT_StartingGridView;
    W.grid->world = W.world;
    W.grid->times = W.times;
    for (int i = 0; i < 8; i++) {
        CarPosition* p = (CarPosition*)wv(sizeof(CarPosition));
        uint8_t* e = W.world + 0x28 + i * 0xc8;
        call_orig(0x00410360, 1, {U(p), 0, U(e + 0x11), U(e + 4), *(uint32_t*)(e + 0xc0), 0, (uint32_t)i});
        W.grid->pos[i] = p;
    }
    W.tpi = (TrackPrizeInfo*)wv(sizeof(TrackPrizeInfo));
    W.tpi->vtbl = (const void*)(uintptr_t)VT_TrackPrizeInfo;
    W.tpi->prizes = W.prizes;
    W.lt = (LapsTable*)wv(sizeof(LapsTable));
    HS->player = 1;
    call_orig(0x0040afe0, 1, {U(W.lt), 0, 4});
    W.prt = (PostRaceTable*)wv(sizeof(PostRaceTable));
    W.prt->vtbl = (const void*)(uintptr_t)VT_PostRaceTable;
    {
        UIStyleDesc* d = (UIStyleDesc*)wv(sizeof(UIStyleDesc));
        d->font = "cour14.fnt"; d->flags = 9; d->c_normal = 0x808080; d->c_over = 0xffffff; d->c_normal2 = 0x404040;
        call_orig(0x0047f4a0, 0, {U(d)});
        W.prt->style = (int32_t)g_setup_ret;
        d->flags = 0xc;
        call_orig(0x0047f4a0, 0, {U(d)});
        W.prt->style_title = (int32_t)g_setup_ret;
    }
    W.prt->count = 4;
    // a full-screen window with each control in a CustomWidget
    UIDialogItem* items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 8);
    UIDialogItem* it = items;
    it = it_(it, 23, 0, 20, 20, 0x69, 0x69, "", 0, W.cp, 0);
    it = it_(it, 23, 0, 120, 160, 0x1e0, 0xe6, "", 0, W.grid, 0);
    it = it_(it, 23, 0, 350, 140, 0x12c, 0xc8, "", 0, W.tpi, 0);
    it = it_(it, 23, 0, 390, 126, 0xcc, 0x108, "", 0, W.lt, 0);
    it = it_(it, 23, 0, 19, 84, 0x142, 0x136, "", 0, W.prt, 0);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    call_orig(F_WidgetCreateWindow, 0, {0, 0, 0, 640, 480, 2});
    W.win = (WidgetWindow*)(uintptr_t)g_setup_ret;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    call_orig(F_UIAddItems, 0, {U(W.win), U(items)});
    const void* ctl[] = {W.cp, W.grid, W.tpi, W.lt, W.prt};
    for (const void* c : ctl) call_orig((*(const uint32_t* const*)c)[1], 1, {U(c), 0});
    for (int i = 0; i < W.win->count; i++) W.widgets.push_back((Widget*)W.win->widgets[i]);
    if (getenv("VP_DEBUG_WORLD"))
        printf("  world: %d widgets; laps table group %08x; styles %d %d\n", W.win->count, W.lt->group, W.prt->style, W.prt->style_title);
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
// every function-local Xlator of this group: {guard, bit, Xlator, key}
static const uint32_t k_xl[][4] = {
    {0x0050409c, 0x01, 0x00504080, 0x004e3af8}, {0x0050409c, 0x02, 0x00504008, 0x004e3b0c}, {0x0050409c, 0x04, 0x00503fd8, 0x004e3b20},
    {0x0050409c, 0x08, 0x00503ff8, 0x004e3b34},
    {0x00505d7c, 0x01, 0x00505e10, 0x004e5788}, {0x00505d7c, 0x02, 0x00505e40, 0x004e57a0}, {0x00505d7c, 0x04, 0x00505de0, 0x004e57b4},
    {0x00505d7c, 0x08, 0x00505cf8, 0x004e57cc}, {0x00505d7c, 0x10, 0x00505c38, 0x004e57e0}, {0x00505d7c, 0x20, 0x00505cc8, 0x004e57f8},
    {0x00505d7c, 0x40, 0x00505d80, 0x004e580c}, {0x00505d7c, 0x80, 0x00505d18, 0x004e5820},
    {0x00505ddc, 0x01, 0x00505cb0, 0x004e5830}, {0x00505ddc, 0x02, 0x00505df0, 0x004e5844}, {0x00505ddc, 0x04, 0x00505e00, 0x004e5858},
    {0x00505ddc, 0x08, 0x00505dd0, 0x004e5868}, {0x00505ddc, 0x10, 0x00505c90, 0x004e587c}, {0x00505ddc, 0x20, 0x00505ce8, 0x004e5894},
    {0x00505ddc, 0x40, 0x00505d08, 0x004e58a8}, {0x00505ddc, 0x80, 0x00505cd8, 0x004e58c0},
    {0x00505d28, 0x01, 0x00505e20, 0x004e58d0}, {0x00505d28, 0x02, 0x00505ca0, 0x004e58e4},
    {0x005d5858, 0x01, 0x005d5860, 0x004e59dc}, {0x005d5858, 0x02, 0x005d5870, 0x004e59c8}, {0x005d5858, 0x04, 0x005d5880, 0x004e59b4},
    {0x0050592c, 0x01, 0x00505958, 0x004e5040}, {0x0050592c, 0x02, 0x00505998, 0x004e505c}, {0x0050592c, 0x04, 0x00505920, 0x004e5070},
    {0x0050592c, 0x08, 0x005058c0, 0x004e5080}, {0x0050592c, 0x10, 0x00505938, 0x004e5094},
    {0x005059b4, 0x01, 0x00505948, 0x004e50dc}, {0x005059b4, 0x02, 0x00505900, 0x004e50f0}, {0x005059b4, 0x04, 0x00505988, 0x004e50f8},
    {0x005059b4, 0x08, 0x005059a8, 0x004e5110}, {0x005059b4, 0x10, 0x005058f0, 0x004e5128}, {0x005059b4, 0x20, 0x00505910, 0x004e5144},
    {0x005059b4, 0x40, 0x00505970, 0x004e5158},
    {0x005d589c, 0x01, 0x005d58f0, 0x004e5220}, {0x005d589c, 0x02, 0x005d58c0, 0x004e5208}, {0x005d589c, 0x04, 0x005d58b0, 0x004e51f4},
    {0x005d589c, 0x08, 0x005d5890, 0x004e51e4}, {0x005d589c, 0x10, 0x005d58d0, 0x004e51d0}, {0x005d589c, 0x20, 0x005d58e0, 0x004e51b8},
    {0x005d589c, 0x40, 0x005d58a0, 0x004e51a0},
};
static const char k_chars[] = "abcdefghij klmnopqrstuvwxyz ABCDEFGH 0123456789 .,!-";
static void random_text(char* s, int maxlen) {
    const int n = irange(0, maxlen);
    for (int i = 0; i < n; i++) s[i] = k_chars[rnd() % (sizeof k_chars - 1)];
    s[n] = 0;
}
static uint32_t time_bits() {
    switch (rnd() % 6) {
    case 0: return 0;
    case 1: return chance(50) ? 0x80000000u : 0x33800000u;           // -0, tiny (below the epsilon)
    case 2: return fbits(wildf());
    default: return fbits(range(20.0f, 400.0f));
    }
}
static void randomize_world() {
    uint8_t* w = W.world;
    const int32_t n = chance(90) ? irange(1, 8) : irange(0, 12);
    *(int32_t*)(w + 0xca8) = n;
    for (int i = 0; i < 16; i++) {
        uint8_t* e = w + 0x28 + i * 0xc8;
        char d[16];
        random_text(d, 11);
        fill_world_car(e, chance(70) ? 1 : irange(0, 3), chance(80) ? k_cars[rnd() % 8] : "x", d, irange(-3, 9));
        if (chance(20)) *(int32_t*)e = 0;
    }
    if (n > 0 && chance(30)) *(int32_t*)(w + 0x28 + (n - 1) * 0xc8) = 3;
    strcpy((char*)w + 8, chance(90) ? k_tracks[rnd() % 9] : "nowhere");
    *(int32_t*)(w + 0xcac) = irange(0, 2);
    *(int32_t*)(w + 0xcbc) = irange(0, 7);
    *(int32_t*)(w + 0xcc0) = irange(0, 30);
    w[0xcd0] = (uint8_t)(rnd() & 1);
    w[0xcd1] = (uint8_t)(rnd() & 1);
    for (int i = 0; i < 16; i++) W.times[i] = time_bits();
    for (int i = 0; i < 16; i++) W.prizes[i] = chance(10) ? 0 : irange(-5, 100000);
    for (int i = 0; i < 0x31; i++) W.res[i] = (int32_t)rnd();
    for (int i = 0; i < 64; i++) W.mblock[i] = fval(-2, 2);
    for (int i = 0; i < 12; i++) W.frame[i] = fval(-1, 1);
    if (chance(40)) {                                    // a rotation, now and then
        const float a = range(-3.2f, 3.2f), b = range(-3.2f, 3.2f);
        const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        const float m[9] = {ca, 0, -sa, sa * sb, cb, ca * sb, sa * cb, -sb, ca * cb};
        memcpy(W.frame, m, sizeof m);
        if (chance(30)) { W.frame[0] = 1; W.frame[1] = 0; W.frame[2] = 0; W.frame[3] = 0; W.frame[4] = 1; W.frame[5] = 0; W.frame[6] = 0; W.frame[7] = 0; W.frame[8] = 1; }
    }
    for (int i = 0; i < 6; i++) W.cframe[i] = fval(-3, 3);
    if (chance(20)) W.cframe[3] = W.cframe[4] = W.cframe[5] = chance(50) ? 0.0f : 1e-5f;
    for (int i = 0; i < 8; i++) random_text(W.texts[i], i < 4 ? 20 : 40);
    for (int i = 0; i < 8; i++) W.flags[i] = (uint8_t)rnd();
    // the objects
    CarPosition* c = W.cp;
    c->time = time_bits();
    c->index = chance(85) ? irange(0, 7) : irange(-3, 12);
    c->odd = (uint8_t)(chance(90) ? rnd() & 1 : rnd());
    c->player = (uint8_t)(chance(90) ? rnd() & 1 : rnd());
    c->driver = W.texts[rnd() % 4];
    c->x = irange(-50, 600); c->y = irange(-50, 460); c->w = irange(-10, 200); c->h = irange(-10, 200);
    for (int i = 0; i < 12; i++) ((uint32_t*)&c->frame)[i] = fbits(fval(-2, 2));
    g_shadow->w = irange(-5, 120); g_shadow->h = irange(-5, 60);
    W.grid->times = chance(80) ? W.times : 0;
    W.tpi->prizes = chance(90) ? W.prizes : 0;
    LapsTable* t = W.lt;
    t->cars = chance(95) ? irange(1, 9) : irange(-2, 0);
    t->car = chance(90) ? irange(0, 8) : irange(-3, 12);
    t->laps = chance(80) ? irange(0, 40) : irange(-5, 200);
    t->page = chance(85) ? irange(0, 4) : irange(-3, 9);
    t->pages = chance(85) ? irange(1, 4) : irange(-2, 9);
    if (chance(10)) t->group = rnd();
    W.prt->count = chance(90) ? irange(0, 8) : irange(-2, 12);
    for (Widget* g : W.widgets) { g->visible = chance(85); g->enabled = chance(85); g->dirty = chance(50); }
    for (int i = 0; i < 16; i++) {
        RbRaceRecord* r = &g_recs[i];
        r->year = (int16_t)irange(1990, 2030); r->day = (uint8_t)irange(1, 31); r->month = (uint8_t)irange(1, 12);
        r->lap_time = chance(20) ? 0 : time_bits(); r->top_speed = chance(20) ? 0 : fbits(fval(-5, 250)); r->race_time = chance(20) ? 0 : time_bits();
        random_text(r->car_name, 12);
        random_text(r->driver_name, 20);
        r->flags = (uint8_t)rnd();
    }
    *(int32_t*)(g_opts + 0x10) = irange(0, 7);
    // the stubs' world
    HS->time = (int32_t)rnd();
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->grab_fail_at = chance(20) ? irange(1, 6) : -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->script_n = HS->script_pos = 0;
    HS->rs = rnd() | 1;
    for (int i = 0; i < 8; i++) HS->scan_mask[i] = chance(30) ? rnd() : rnd() & rnd() & rnd();
    HS->menu_calls = 0; HS->menu_end = irange(1, 5);
    HS->state_calls = 0; HS->state_end = irange(1, 40);
    HS->player = chance(80) ? irange(0, 7) : irange(-2, 9);
    if (chance(10)) HS->player = -1;
    HS->car_count = irange(0, 8);
    HS->tab_rows = chance(90) ? 9 : irange(0, 16);
    HS->deity_laps = irange(0, 30);
    for (int i = 0; i < 16; i++) { HS->laps[i] = chance(20) ? 0 : irange(0, 35); HS->place[i] = irange(0, 9); }
    HS->rec_mask = chance(10) ? rnd() : chance(50) ? 0xffffffffu : ~(rnd() & rnd() & rnd() & rnd());
    HS->find_n = chance(10) ? 0 : irange(1, 20);
    HS->find_pos = 0;
    for (int i = 0; i < 24; i++) {
        char nm[32];
        random_text(nm, 12);
        for (char* p = nm; *p; p++) if (*p == '.' || *p == '\\') *p = 'q';
        if (chance(50)) sprintf(HS->find_names[i], "%s.car", k_cars[rnd() % 8]);
        else if (chance(50)) sprintf(HS->find_names[i], "cars\\%s.car", nm);
        else sprintf(HS->find_names[i], "%s.car", nm);
    }
    HS->opt_mask = rnd();
    for (int i = 0; i < 8; i++) HS->opt_val[i] = chance(85) ? irange(0, 12) : (int32_t)rnd();
    HS->cl_len = fbits(chance(90) ? range(500.0f, 9000.0f) : wildf());
    HS->userdir_len = chance(70) ? irange(3, 150) : chance(50) ? irange(199, 201) : irange(190, 260);
    HS->board_next = 0;
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
    UI_GP(uint8_t, S_LOCALE) = W.locale;
    *(float*)(W.locale + 0x14) = chance(50) ? 1.0f : fval(0, 3);
    *(float*)(W.locale + 0x1c) = chance(50) ? 1.0f : fval(0, 3);
    W.locale[0x38] = (uint8_t)(chance(50) ? 0 : rnd());
    UI_GP(void, S_DEITY) = g_deity;
    UI_GP(char, S_CARLIST) = W.carlist;
    UI_G32(S_CARLIST_N) = chance(90) ? irange(1, 8) : irange(0, 31);
    UI_GP(void, S_TRACK_TAB) = g_tab;
    UI_GP(LapsTable, S_LAPS_GLOBAL) = chance(97) ? W.lt : 0;
    // main.obj's
    UI_G32(S_APP_MODE) = chance(80) ? irange(0, 4) : irange(-1, 6);
    UI_G32(S_CAMERA) = irange(0, 14);
    UI_G32(S_DASH) = chance(95) ? irange(0, 4) : 0;
    UI_G8(S_MAIN_DONE) = (uint8_t)(chance(80) ? 0 : 1);
    UI_G8(S_PAUSED) = (uint8_t)(rnd() & 1);
    UI_G8(S_RESTART) = (uint8_t)(chance(80) ? 0 : 1);
    UI_G8(S_BLIMP_TV) = (uint8_t)(rnd() & 1);
    UI_G8(S_BLIMP_GUARD) = (uint8_t)(chance(80) ? 3 : rnd() & 3);
    for (int i = 0; i < 3; i++) { UI_GF(S_BLIMP_VEL + 4 * i) = fval(-10, 10); UI_GF(S_BLIMP_ROT + 4 * i) = fval(-0.2f, 0.2f); }
    for (int i = 0; i < 12; i++) UI_GF(S_BLIMP_FRAME + 4 * i) = fval(-1, 1);
    for (int i = 0; i < 12; i++) UI_GF(S_BLIMP_START + 4 * i) = fval(-1, 1);
    random_text((char*)(uintptr_t)S_BLIMP_TRACK, 20);
    UI_G32(0x004ec9dc) = irange(0, 5);                 // the TV camera in use, how many (blimp_to_tv)
    UI_G32(0x00521084) = irange(0, 6);
    UI_G8(S_ESC_GUARD) = (uint8_t)(chance(50) ? 0xff : rnd());
    UI_GP(void, S_W32_SPLASH) = chance(50) ? 0 : (void*)(uintptr_t)0x00badbad;
    random_text((char*)(uintptr_t)S_W32_USERDIR, 30);
    for (int d = 0; d < 5; d++) {
        DashType* t = (DashType*)(uintptr_t)(S_DASH_TABLE + 0x14u * (uint32_t)d);
        t->update = chance(50) ? &stub_dash_update : 0;
        t->draw2d = chance(60) ? (void(__cdecl*)(void*)) & stub_dash_draw2d : 0;
        t->draw3d = chance(50) ? &stub_dash_draw3d : 0;
        t->key = chance(50) ? (uint8_t(__cdecl*)(uint32_t)) & stub_dash_key : 0;
        const int nk = irange(0, 10);
        for (int k = 0; k < nk; k++) W.dash_keys[d][k] = (uint8_t)irange(1, 0xfe);
        W.dash_keys[d][nk] = 0xff;
        t->keys = chance(70) ? W.dash_keys[d] : 0;
    }
    const bool all_built = chance(50);
    for (auto& x : k_xl) {                              // each function-local Xlator built (fresh or stale) or not
        if (all_built || chance(75)) {
            UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) | x[1]);
            UI_GU32(x[2]) = x[3];
            UI_GU32(x[2] + 4) = U(xl_text(x[3]));
            UI_GU32(x[2] + 8) = chance(70) ? UI_GU32(S_XLATOR_COOKIE) : ~UI_GU32(S_XLATOR_COOKIE);
        } else {
            UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) & ~x[1]);
        }
    }
}
static void random_script(int maxops) {
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x121, 0x122, 0x124, 0x170, 0x171,
                                    0x175, 0x17a, 0x17b, 0x37b, 'p', 'P', 'g', 'q', 0x123};
    const int n = irange(0, maxops);
    for (int i = 0; i < n && i < 62; i++) {
        int32_t* op = HS->script[i];
        const uint32_t r = rnd() % 10;
        op[1] = op[2] = 0;
        if (r < 3) { op[0] = OP_MOVE_W; op[1] = irange(0, 40); op[2] = irange(0, 479); }
        else if (r < 4) { op[0] = OP_MOVE_XY; op[1] = irange(-10, 650); op[2] = irange(-10, 490); }
        else if (r < 5) op[0] = OP_DOWN;
        else if (r < 6) op[0] = OP_UP;
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
        {0x004e38ec, 0x004e38f8}, {0x004e3900, 0x004e3bb0},             // main.obj's strings
        {0x004e4640, 0x004e4860},                                       // race.obj's strings and the laps table
        {0x004e4db8, 0x004e4dbc},                                       // ""
        {0x004e4ff8, 0x004e5230}, {0x004e5778, 0x004e5a20},             // postrace, prerace, version
        {0x004e5f80, 0x004e5fb8},                                       // win32.obj's
        {0x004eb400, 0x004eb444},                                       // base64.obj's alphabet
        {0x004f665c, 0x004f6698}, {0x004f6b78, 0x004f7228}, {0x004f7230, 0x004f7300},   // the ui library's keys and strings
        {0x004efbf8, 0x004efbfc},
        {0x004f9000, 0x004f9300},                                       // board.obj's (BoardCustomText)
        {0x005024ac, 0x005024c8},                                       // the CRT's FPU flags
        {0x00502630, 0x00502680}, {0x005026f8, 0x00502730}, {0x00502780, 0x005028c8},   // its jump tables and constants
        {0x005228d4, 0x005228d8}, {0x005228f4, 0x005228f8},
        {0x00578cd0, 0x00578cdc}, {0x00578d50, 0x00578d5c}, {0x00578d60, 0x00578d6c}, {0x00578e88, 0x00578e94},
        {0x00578e98, 0x00578ea4}, {0x00578eb0, 0x00578ebc}, {0x00578ec0, 0x00578ecc}, {0x00578ef8, 0x00578f04},
        {0x00579050, 0x0057905c},
        {0x00578d08, 0x00578d40},                                       // the end item
        {0x00578d48, 0x00578d4c}, {0x00578e78, 0x00578e7c}, {0x00578f40, 0x00578f44}, {0x00578e80, 0x00578e84},
        {0x00579730, 0x00579778},                                       // the running window, the window table
    };
    for (auto& k : keep) memcpy(DATA + (k[0] - 0x004e1000), keepfrom.data + (k[0] - 0x004e1000), k[1] - k[0]);
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static uint32_t b8(uint32_t v) { return chance(50) ? (rnd() & ~0xffu) | (v & 0xff) : v; }
static uint32_t canvas_arg() { return chance(70) ? U(W.screen) : U(W.canvas2); }
static uint32_t mat_arg() { return U(W.mblock + (chance(70) ? 9 * (rnd() % 6) : rnd() % 54)); }
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const char* nm = f.name;
    uint32_t* a = f.fast ? w + 2 : w;
    memset(W.scratch, 0xa5, 0x200);
#define IS(s) (!strcmp(nm, s))
#define HAS(s) (strstr(nm, s) != 0)
    if (HAS("deleting destructor")) a[0] = rnd() & 3;
    if (IS("WinMain")) { a[0] = rnd(); a[1] = 0; a[2] = U(W.cmdlines[rnd() % 12]); a[3] = rnd(); }
    else if (IS("AppProcessArgs")) a[0] = chance(95) ? U(chance(20) ? W.texts[rnd() % 8] : W.cmdlines[rnd() % 12]) : 0;
    else if (IS("setup_blimp_jump")) a[0] = U(W.locs[rnd() % 8]);
    else if (IS("convert_frame(Frame&,CompactFrame const&)")) { a[0] = U(W.frame_out); a[1] = U(W.cframe); }
    else if (IS("convert_frame(CompactFrame&,Frame const&)")) { a[0] = U(W.cframe_out); a[1] = U(W.frame); }
    else if (IS("is_pause_key")) a[0] = chance(40) ? (rnd() & 0xffff0000u) | (chance(50) ? 0x50u : 0x70u) : rnd();
    else if (IS("is_pause_scankey")) a[0] = chance(40) ? (rnd() & 0xffffff00u) | 0x19u : rnd();
    else if (IS("MainSetCamera")) a[0] = (uint32_t)irange(-1, 14);
    else if (IS("DoRace")) { a[0] = U(W.world); a[1] = chance(70) ? U(W.res) : 0; a[2] = chance(30) ? U(&stub_post) : 0; }
    else if (IS("set_dash_type")) a[0] = (uint32_t)irange(0, 4);
    else if (IS("escape_callback")) a[0] = (uint32_t)irange(-1, 4);
    else if (HAS("MatrixMake")) { a[0] = mat_arg(); a[1] = chance(20) ? fbits(wildf()) : fbits(range(-7.0f, 7.0f)); }
    else if (IS("MatrixConcat")) {
        a[0] = mat_arg(); a[1] = mat_arg(); a[2] = mat_arg();
        switch (rnd() % 5) { case 0: a[1] = a[0]; break; case 1: a[2] = a[0]; break; case 2: a[1] = a[2] = a[0]; break; }
    }
    else if (IS("GetRealismString") || IS("GetWeatherString") || IS("GetGameTimeString")) a[0] = (uint32_t)irange(0, 2);
    else if (IS("GetRaceTypeString")) a[0] = (uint32_t)irange(0, 7);
    else if (IS("GetGhostCarTypeString")) a[0] = (uint32_t)irange(0, 4);
    else if (IS("GetAIStrengthString")) a[0] = (uint32_t)irange(0, 7);
    else if (IS("GetFieldString")) a[0] = (uint32_t)irange(0, 5);
    else if (IS("GetEventString")) a[0] = (uint32_t)irange(0, 6);
    else if (IS("GetCameraName")) a[0] = (uint32_t)irange(0, 11);
    else if (IS("GetLapCountFromType")) { a[0] = (uint32_t)irange(0, 7); a[1] = chance(90) ? U(k_tracks[rnd() % 9]) : U(W.texts[0]); }
    else if (IS("GetTrackDifficulty")) a[0] = (uint32_t)irange(0, 7);
    else if (IS("sort_f")) { a[0] = U(W.carlist + 32 * (rnd() % 8)); a[1] = U(W.carlist + 32 * (rnd() % 8)); }
    else if (IS("GetTrackName") || IS("GetTrackFriendlyName") || IS("GetTrackText")) a[0] = (uint32_t)irange(-1, 12);
    else if (IS("GetTrackNumber")) a[0] = chance(85) ? U(k_tracks[rnd() % 9]) : U(W.texts[rnd() % 4]);
    else if (IS("GetCarFileName")) a[0] = (uint32_t)irange(0, 31);
    else if (IS("GetCarFileNumber")) a[0] = chance(85) ? U(k_cars[rnd() % 8]) : U(W.texts[rnd() % 4]);
    else if (IS("ASSERT_MSG")) { a[0] = rnd(); a[1] = U("bad car index: %d"); }
    else if (IS("PreRaceDo")) {
        a[0] = U(W.world); a[1] = b8(chance(20) ? 1 : 0); a[2] = U(W.flags); a[3] = b8(rnd() & 1);
        a[4] = chance(70) ? U(W.times) : 0; a[5] = chance(70) ? U(W.prizes) : 0;
    }
    else if (IS("CarPosition::CarPosition")) {
        w[0] = U(W.scratch);
        a[0] = U(chance(90) ? k_cars[rnd() % 8] : W.texts[0]); a[1] = U(W.texts[1]); a[2] = (uint32_t)irange(-3, 8); a[3] = time_bits();
        a[4] = (uint32_t)irange(-1, 9);
    }
    else if (HAS("CarPosition::")) { w[0] = U(W.cp); if (HAS("Draw") && !HAS("3D")) a[0] = canvas_arg(); }
    else if (HAS("StartingGridView::")) { w[0] = U(W.grid); if (HAS("::Draw")) a[0] = canvas_arg(); }
    else if (HAS("TrackPrizeInfo::") && !HAS("XL_")) { w[0] = U(W.tpi); if (HAS("::Draw")) a[0] = canvas_arg(); }
    else if (IS("LapsTable::LapsTable")) { w[0] = U(W.scratch); a[0] = (uint32_t)irange(-1, 9); }
    else if (IS("LapsTable::NextCar") || IS("LapsTable::PrevCar") || IS("LapsTable::NextPage") || IS("LapsTable::PrevPage")) a[0] = rnd();
    else if (HAS("LapsTable::")) {
        w[0] = U(W.lt);
        if (HAS("::Draw")) a[0] = canvas_arg();
        else if (HAS("Callback")) { a[0] = rnd() & 3; a[1] = U(&W.lt->page); }
    }
    else if (IS("PostRaceDo")) a[0] = U(W.world);
    else if (HAS("PostRaceTable::") && !HAS("XL_")) { w[0] = U(W.prt); if (HAS("::Draw")) a[0] = canvas_arg(); }
#undef IS
#undef HAS
    return true;
}
static bool is_modal(const char* nm) {
    static const char* const m[] = {"PreRaceDo", "PostRaceDo", "DoRace", "game_loop", "WinMain", "AppMain", "race", "blimp_jump",
                                    "dave_b", "rich_g"};
    for (const char* s : m) if (!strcmp(nm, s)) return true;
    return false;
}

// ---- the fix build: rounds kept out of the comparison ------------------------------------------------------------------------
// (before the original runs) CarPosition::CarPosition with a car name of 12 or more characters: the fixed remap names stop at
// their 16 bytes, where the original's ran on over fields it then wrote again (so they differ in bytes nothing reads)
static bool fx_pre_case(const Ent& f, const uint32_t* w) {
    if (!strcmp(f.name, "CarPosition::CarPosition")) {
        const char* car = (const char*)(uintptr_t)w[2];              // (ecx, edx, then the car)
        return readable(car, 1) && strnlen(car, 12) > 11;
    }
    return false;
}

#if ROOT_FIXES
// ---- the fixes, each on its bad case (directed_fix_tests) -----------------------------------------------------------------
// The rewrite alone (the original would crash or overrun there -- this program's stack among what it would take), from the
// pristine world with the case set up: it must return cleanly (no fault, the bytes popped, ebx / esi / edi / ebp kept),
// write nothing outside what it may (every other byte of .data/.bss/.idata and the arena compared with before) and give
// what the fix promises. Each fix's boundary case (the longest input that fits) runs on the original and the rewrite from
// the same state and must give the same memory, call logs and result.
static int g_fx_bad, g_fx_n, g_fx_same_n;
static const Ent& fx_fn(const char* name) {
    for (int i = 0; i < g_nfns; i++)
        if (!strcmp(g_fns[i].name, name)) return g_fns[i];
    printf("  fix test: %s isn't listed\n", name);
    fflush(stdout);
    ExitProcess(4);
}
struct Span { const void* p; uint32_t n; };
static uint32_t fx_outside_in(const Mem& before, bool (*in)(const uint8_t*, const void*), const void* ctx) {
    for (uint32_t i = 0; i < DATA_BYTES; i++)
        if (before.data[i] != DATA[i] && !in(DATA + i, ctx)) return 0x004e1000 + i;
    for (uint32_t i = 0; i < IDATA_BYTES; i++)
        if (before.idata[i] != IDATA[i] && !in(IDATA + i, ctx)) return 0x005d7000 + i;
    for (uint32_t i = sizeof(HState); i < ARENA_BYTES; i++)
        if (before.arena[i] != g_arena[i] && !in(g_arena + i, ctx)) return U(g_arena + i);
    return 0;
}
// the first byte that changed since `before` outside the spans (the stubs' state block aside); 0 if none
static uint32_t fx_outside(const Mem& before, std::initializer_list<Span> ok) {
    struct C { const Span* b; const Span* e; } c = {ok.begin(), ok.end()};
    return fx_outside_in(before, [](const uint8_t* q, const void* x) {
        const C* c = (const C*)x;
        for (const Span* sp = c->b; sp != c->e; sp++)
            if (q >= (const uint8_t*)sp->p && q < (const uint8_t*)sp->p + sp->n) return true;
        return false;
    }, &c);
}
// ... outside the function's footprint
static uint32_t fx_outside_fp(const Mem& before, const Footprint& fp) {
    return fx_outside_in(before, [](const uint8_t* q, const void* x) {
        const Footprint* fp = (const Footprint*)x;
        for (int k = 0; k < fp->n; k++)
            if (q >= (const uint8_t*)fp->r[k].p && q < (const uint8_t*)fp->r[k].p + fp->r[k].n) return true;
        return false;
    }, &fp);
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
// a callee replaced for a test (a jump to a recorder), put back after
struct FxPatch {
    uint32_t at;
    uint8_t b[5];
    FxPatch(uint32_t a, void* to) : at(a) { memcpy(b, (const void*)(uintptr_t)a, 5); patch_jmp(a, to); }
    ~FxPatch() { memcpy((void*)(uintptr_t)at, b, 5); }
};
static char g_fx_dir[0x200];
static int g_fx_dir_n;
static uint8_t __cdecl fx_FileChangeDir(const char* d) { L('FCHD'); LS(d); rec(g_fx_dir, sizeof g_fx_dir, d); g_fx_dir_n++; return 1; }
static uint8_t g_fx_world[0x40];
static void __cdecl fx_LoadRace(const uint8_t* w) { L('LDRC'); memcpy(g_fx_world, w, sizeof g_fx_world); }
static char g_fx_wrap[0x200];
static uint32_t g_fx_wrap_len;
static void __cdecl fx_WordWrap(int32_t, int32_t, char* out, const char* in) {
    L('WRAP');
    g_fx_wrap_len = (uint32_t)strlen(in);
    rec(g_fx_wrap, sizeof g_fx_wrap, in);
    rec(out, 0x100, in);
}
// the tests' world: pristine, the stubs' hooks off, every function-local Xlator built (fresh), the statics the code follows
static void fx_reset() {
    mem_load(g_pristine);
    g_fx_xl = g_fx_xl_name = g_fx_xl_text = 0;
    g_fx_find = 0;
    g_fx_find_n = 0;
    g_fx_real_tex = false;
    g_xl_seed = 0;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    UI_GU32(S_DIALOG_IDLE) = 0;
    UI_G8(S_EXIT) = 0;
    UI_GF(S_UI_DT) = 0.05f;
    UI_GP(void, S_CURSOR) = 0;
    UI_G32(S_MOUSE_X) = 320; UI_G32(S_MOUSE_Y) = 240;
    UI_GP(uint8_t, S_LOCALE) = W.locale;
    *(float*)(W.locale + 0x14) = 1.0f;
    *(float*)(W.locale + 0x1c) = 1.0f;
    W.locale[0x38] = 0;
    UI_GP(void, S_DEITY) = g_deity;
    UI_GP(char, S_CARLIST) = W.carlist;
    UI_G32(S_CARLIST_N) = 8;
    UI_GP(void, S_TRACK_TAB) = g_tab;
    for (auto& x : k_xl) {
        UI_G8(x[0]) = (uint8_t)(UI_G8(x[0]) | x[1]);
        UI_GU32(x[2]) = x[3];
        UI_GU32(x[2] + 4) = U(xl_text(x[3]));
        UI_GU32(x[2] + 8) = UI_GU32(S_XLATOR_COOKIE);
    }
    HS->time = 1000; HS->frames = 0; HS->frame_limit = 40; HS->grab_fail_at = -1;
    HS->mouse_x = 320; HS->mouse_y = 240;
    HS->script_n = HS->script_pos = 0;
    HS->rs = 0x1234567;
    for (int i = 0; i < 8; i++) HS->scan_mask[i] = 0;
    HS->menu_calls = 0; HS->menu_end = 2;
    HS->state_calls = 0; HS->state_end = 3;
    HS->player = 0; HS->car_count = 2; HS->tab_rows = 9; HS->deity_laps = 3;
    HS->find_n = 0; HS->find_pos = 0; HS->board_next = 0;
    UI_G8(0x004eb444) = 1;                                      // base64.obj: its table built on first use
    HS->rec_mask = 0xffffffffu;
    for (int i = 0; i < 16; i++) { HS->laps[i] = 3; HS->place[i] = i; }
    HS->cl_len = 0x45800000u;                                   // 4096
    HS->userdir_len = 20;
    UI_G32(S_APP_MODE) = 0;
    UI_G32(S_CAMERA) = 5;
    UI_G32(S_DASH) = 0;
    UI_G8(S_MAIN_DONE) = 0; UI_G8(S_PAUSED) = 0; UI_G8(S_RESTART) = 0; UI_G8(S_BLIMP_TV) = 0;
    UI_G8(S_BLIMP_GUARD) = 3;
    UI_G8(S_ESC_GUARD) = 0xff;
    UI_GP(void, S_W32_SPLASH) = 0;
    for (int d = 0; d < 5; d++) {                               // the dashboards: the stubs
        DashType* t = (DashType*)(uintptr_t)(S_DASH_TABLE + 0x14u * (uint32_t)d);
        t->update = &stub_dash_update;
        t->draw2d = (void(__cdecl*)(void*)) & stub_dash_draw2d;
        t->draw3d = &stub_dash_draw3d;
        t->key = (uint8_t(__cdecl*)(uint32_t)) & stub_dash_key;
        W.dash_keys[d][0] = 0xff;
        t->keys = W.dash_keys[d];
    }
}
static char g_fx_s[8][0x400];
// a string of n c's and then tail (in one of eight buffers of this program's)
static const char* mk(int k, char c, int n, const char* tail = "") {
    memset(g_fx_s[k], c, (size_t)n);
    strcpy(g_fx_s[k] + n, tail);
    return g_fx_s[k];
}
// s's first n characters
static std::string first(const char* s, size_t n) { return std::string(s, strnlen(s, n)); }
static const uint32_t k_identity[12] = {0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0};

static int directed_fix_tests() {
    Result r;
    uint32_t o;
    char m[512];
    static Footprint fp;
    auto footprint = [&](const Ent& f, std::initializer_list<uint32_t> w) {
        uint32_t words[72] = {};
        int i = 0;
        for (uint32_t x : w) words[i++] = x;
        fp.n = 0; fp.replay_only = 0; fp.pure = false;
        f.fp(fp, words);
    };

    // ---- 2. GetEventString: long translations ----
    {
        const Ent& f = fx_fn("GetEventString");
        struct { const char* text; uint8_t metric; float k; int32_t n0, n1; const char* what; } cases[] = {
            {mk(0, 'e', 60), 0, 1.0f, 60, 100, "a 60-character translation"},
            {mk(1, 'f', 20), 1, 1.2e8f, 0, 0, "a 20-character translation and a big locale factor's number (11 characters)"},
            {mk(2, 'g', 300), 0, 1.0f, 60, 100, "a 300-character translation"},
        };
        for (auto& c : cases) {
            if (c.metric) {                                     // the numbers as __ftol makes them: the 64-bit integer's low dword
                float k0, k1;
                const uint32_t b0 = 0x41d55555, b1 = 0x4231c71c;
                memcpy(&k0, &b0, 4);
                memcpy(&k1, &b1, 4);
                c.n0 = (int32_t)(uint32_t)(uint64_t)(int64_t)((double)c.k * (double)k0);
                c.n1 = (int32_t)(uint32_t)(uint64_t)(int64_t)((double)c.k * (double)k1);
            }
            fx_reset();
            g_fx_xl = c.text;
            for (uint32_t a : root_race::k_xl_event) UI_GU32(a + 8) = ~UI_GU32(S_XLATOR_COOKIE);    // stale: refreshed with it
            W.locale[0x38] = c.metric;
            *(float*)(W.locale + 0x1c) = c.k;
            footprint(f, {1});
            mem_save(g_snap);
            r = fx_run(f, true, {1});
            sprintf(m, "GetEventString, %s: a clean return", c.what);
            fx_check(fx_clean(f, r), m, &r);
            o = fx_outside_fp(g_snap, fp);
            sprintf(m, "GetEventString, %s: nothing written outside its three texts and Xlators", c.what);
            fx_check(o == 0, m, 0, o);
            char e[3][0x200];
            sprintf(e[0], "%s %d", c.text, c.n0);
            sprintf(e[1], "%s %d", c.text, c.n1);
            sprintf(e[2], "%d %s", c.n1, c.text);
            const uint32_t at[3] = {S_EVENT_TEXT0, S_EVENT_TEXT1, S_EVENT_TEXT2};
            for (int k = 0; k < 3; k++) {
                sprintf(m, "GetEventString, %s: text %d is the first 31 characters of \"%.40s...\"", c.what, k, e[k]);
                fx_check(first(e[k], 31) == std::string((const char*)(uintptr_t)at[k]), m);
            }
            fx_check(r.ret == S_EVENT_TEXT0, "GetEventString(1): the quarter mile's text");
        }
        // the boundary: 27 characters ("<27> 100" and "100 <27>" are 31)
        fx_reset();
        g_fx_xl = mk(0, 'e', 27);
        for (uint32_t a : root_race::k_xl_event) UI_GU32(a + 8) = ~UI_GU32(S_XLATOR_COOKIE);
        fx_same(f, {5}, "GetEventString, a 27-character translation (texts of 31 characters)");
    }

    // ---- 3. RaceBegin: 40 cars; long names ----
    {
        const Ent& f = fx_fn("RaceBegin");
        static char names[40][0x80];
        static const char* ptrs[40];
        for (int i = 0; i < 40; i++) {
            sprintf(names[i], i % 3 == 0 ? "cars\\car%02d.car" : "car%02d.car", 39 - i);
            ptrs[i] = names[i];
        }
        fx_reset();
        g_fx_find = ptrs;
        g_fx_find_n = 40;
        mem_save(g_snap);
        r = fx_run(f, true, {});
        fx_check(fx_clean(f, r), "RaceBegin, 40 cars: a clean return", &r);
        const char* list = UI_GP(const char, S_CARLIST);
        const int32_t n = UI_G32(S_CARLIST_N);
        sprintf(m, "RaceBegin, 40 cars: the list holds 32 (%d)", n);
        fx_check(n == 32, m);
        bool ok = readable(list, 0x400);
        for (int k = 0; ok && k < 32; k++) {                    // the first 32 listed (car39 .. car08), sorted
            char e[16];
            sprintf(e, "car%02d", 8 + k);
            ok = !strcmp(list + 32 * k, e);
        }
        fx_check(ok, "RaceBegin, 40 cars: the first 32 the folder lists, sorted");
        o = fx_outside(g_snap, {{(void*)(uintptr_t)S_CARLIST, 8}, {(void*)(uintptr_t)S_TRACK_TAB, 4}, {list, 0x400}});
        fx_check(o == 0, "RaceBegin, 40 cars: nothing written outside the list's 0x400 bytes and its statics", 0, o);
        // names too long for an entry
        static char lng[5][0x100];
        sprintf(lng[0], "viper.car");
        sprintf(lng[1], "%s.car", mk(0, 'a', 31));
        sprintf(lng[2], "cars\\%s.car", mk(1, 'b', 32));
        sprintf(lng[3], "%s.car", mk(2, 'c', 100));
        sprintf(lng[4], "gts.car");
        static const char* lp[5] = {lng[0], lng[1], lng[2], lng[3], lng[4]};
        fx_reset();
        g_fx_find = lp;
        g_fx_find_n = 5;
        mem_save(g_snap);
        r = fx_run(f, true, {});
        fx_check(fx_clean(f, r), "RaceBegin, names of 31, 32 and 100 characters: a clean return", &r);
        list = UI_GP(const char, S_CARLIST);
        fx_check(UI_G32(S_CARLIST_N) == 3 && readable(list, 0x400) && !strcmp(list, mk(3, 'a', 31)) && !strcmp(list + 32, "gts") &&
                     !strcmp(list + 64, "viper"),
                 "RaceBegin, names of 31, 32 and 100 characters: the 31-character name kept, the longer two left out");
        o = fx_outside(g_snap, {{(void*)(uintptr_t)S_CARLIST, 8}, {(void*)(uintptr_t)S_TRACK_TAB, 4}, {list, 0x400}});
        fx_check(o == 0, "RaceBegin, long names: nothing written outside the list and its statics", 0, o);
        // the boundary: 32 names of 31 characters
        static char full[32][0x40];
        static const char* fp32[32];
        for (int i = 0; i < 32; i++) {
            sprintf(full[i], "x%030d.car", 31 - i);
            fp32[i] = full[i];
        }
        fx_reset();
        g_fx_find = fp32;
        g_fx_find_n = 32;
        fx_same(f, {}, "RaceBegin, 32 cars of 31 characters (the list full)");
    }

    // ---- 4. CarPosition::CarPosition: long car names ----
    {
        const Ent& f = fx_fn("CarPosition::CarPosition");
        uint8_t* obj = W.scratch;
        struct { const char* car; int32_t drv; } cases[] = {
            {"abcdefghijkl", 2}, {"a_car_name_of_twenty", -1}, {"a_mod_car_with_a_long_name_of31", -1}, {mk(4, 'z', 60), 3},
        };
        for (auto& c : cases) {
            const size_t len = strlen(c.car);
            std::string cut = first(c.car, 48);
            char mod[0x80], from[0x80], to[0x80];
            sprintf(mod, "%s3.mod", cut.c_str());
            sprintf(from, "%s.tex", cut.c_str());
            sprintf(to, c.drv < 0 ? "~%s.tex" : "%s.tex", cut.c_str());
            // the original, where its frame's names fit (26 characters): the names the model got
            char o_from[17] = {}, o_to[17] = {};
            uint8_t o_tail[8] = {};
            const bool orig = len <= 26;
            if (orig) {
                fx_reset();
                g_fx_real_tex = true;
                memset(obj, 0xa5, 0x200);
                r = fx_run(f, false, {U(obj), 0, U(c.car), U(W.texts[1]), (uint32_t)c.drv, 0, 1});
                memcpy(o_from, g_rec_remap, 16);
                memcpy(o_to, g_rec_remap + 0x10, 16);
                memcpy(o_tail, g_rec_remap + 0x20, 8);
            }
            fx_reset();
            g_fx_real_tex = true;
            memset(obj, 0xa5, 0x200);
            g_rec_remap_load[0] = 0;
            mem_save(g_snap);
            r = fx_run(f, true, {U(obj), 0, U(c.car), U(W.texts[1]), (uint32_t)c.drv, 0, 1});
            sprintf(m, "CarPosition, a car name of %u characters: a clean return", (unsigned)len);
            fx_check(fx_clean(f, r) && r.ret == U(obj), m, &r);
            o = fx_outside(g_snap, {{obj, 0xa4}});
            sprintf(m, "CarPosition, a car name of %u characters: nothing written outside the object", (unsigned)len);
            fx_check(o == 0, m, 0, o);
            sprintf(m, "CarPosition, a car name of %u characters: the model \"%s\"", (unsigned)len, mod);
            fx_check(!strcmp(g_rec_remap_load, mod), m);
            const CarPosition* cp = (const CarPosition*)obj;
            auto name16 = [](const char* field, const char* want) {
                return strlen(want) >= 16 ? !memcmp(field, want, 16) : !strcmp(field, want);
            };
            sprintf(m, "CarPosition, a car name of %u characters: the remap's names \"%.16s\" / \"%.16s\", then zeros", (unsigned)len, from, to);
            fx_check(name16(cp->remap_from, from) && name16(cp->remap_to, to) && cp->remap_20 == 0 && cp->remap_24 == 0, m);
            if (orig) {
                char n_from[17] = {}, n_to[17] = {};
                memcpy(n_from, g_rec_remap, 16);
                memcpy(n_to, g_rec_remap + 0x10, 16);
                sprintf(m, "CarPosition, a car name of %u characters: the model got the original's names (16 characters each)", (unsigned)len);
                fx_check(!strcmp(o_from, n_from) && !strcmp(o_to, n_to) && !memcmp(o_tail, g_rec_remap + 0x20, 8), m);
            }
        }
        // the boundaries: "<11>.tex" and "~<10>.tex" (15 characters)
        fx_reset();
        g_fx_real_tex = true;
        memset(obj, 0xa5, 0x200);
        fx_same(f, {U(obj), 0, U("abcdefghijk"), U(W.texts[1]), 2, 0, 1}, "CarPosition, an 11-character car (\"<car>.tex\" of 15)");
        fx_reset();
        g_fx_real_tex = true;
        memset(obj, 0xa5, 0x200);
        fx_same(f, {U(obj), 0, U("abcdefghij"), U(W.texts[1]), (uint32_t)-1, 0, 1}, "CarPosition, a 10-character car (\"~<car>.tex\" of 15)");
    }

    // ---- 5. PreRaceDo: a long friendly name; a damaged World's track name; long track texts (the frame's 4 KB) ----
    {
        const Ent& f = fx_fn("PreRaceDo");
        uint8_t* w = W.world;
        const std::initializer_list<uint32_t> args = {U(w), 0, U(W.flags), 0, U(W.times), U(W.prizes)};
        uint8_t g_trk[8], g_fr[4], g_trk2[8];
        // a track text of n characters, words of five
        static char big[0x1100];
        auto words = [](int n) {
            for (int i = 0; i < n; i++) big[i] = i % 6 == 5 ? ' ' : (char)('a' + i % 23);
            big[n] = 0;
            return (const char*)big;
        };
        for (int pass = 0; pass < 2; pass++) {                  // with UIStyleWordWrap recorded, then the game's own
            fx_reset();
            strcpy((char*)w + 8, "uptown");
            *(int32_t*)(w + 0xca8) = 2;
            g_fx_xl_name = mk(0, 'n', 100);
            g_fx_xl_text = pass == 0 ? mk(1, 't', 400, " the end") : words(4000);
            memcpy(g_trk, (const void*)0x00505c88, 8);
            memcpy(g_fr, (const void*)0x00505dd0, 4);
            g_fx_wrap_len = 0;
            if (pass == 0) {
                FxPatch wrap(0x0047fb90, (void*)&fx_WordWrap);
                r = fx_run(f, true, args);
            } else {
                r = fx_run(f, true, args);
            }
            sprintf(m, "PreRaceDo, a 100-character friendly name and a %s: a clean return",
                    pass ? "4000-character text (the game's word wrap: the frame holds it)" : "408-character text (the word wrap recorded)");
            fx_check(fx_clean(f, r), m, &r);
            fx_check(first(g_fx_xl_name, 63) == std::string((const char*)(uintptr_t)S_PR_FRIENDLY),
                     "PreRaceDo: the friendly name's first 63 characters");
            fx_check(!strcmp((const char*)(uintptr_t)S_PR_TRACK, "uptown"), "PreRaceDo: the track's name");
            fx_check(!memcmp(g_trk, (const void*)0x00505c88, 8) && !memcmp(g_fr, (const void*)0x00505dd0, 4),
                     "PreRaceDo: the static after the track's name and the Xlator after the friendly name untouched");
            if (pass == 0) {
                sprintf(m, "PreRaceDo: the track text is wrapped whole, as the original does (%u characters)", g_fx_wrap_len);
                fx_check(g_fx_wrap_len == 408 && std::string(g_fx_xl_text) == std::string(g_fx_wrap), m);
            }
        }
        // a damaged World: its track name 70 characters, over the (no) cars
        fx_reset();
        memset(w + 8, 'w', 70);
        w[8 + 70] = 0;
        *(int32_t*)(w + 0xca8) = 0;
        memcpy(g_trk2, (const void*)0x00505c88, 8);
        r = fx_run(f, true, args);
        fx_check(fx_clean(f, r), "PreRaceDo, a 70-character track name: a clean return", &r);
        fx_check(std::string((const char*)(uintptr_t)S_PR_TRACK) == std::string(63, 'w') && !memcmp(g_trk2, (const void*)0x00505c88, 8),
                 "PreRaceDo, a 70-character track name: its first 63 characters, the static after it untouched");
        // the boundaries: a friendly name of 63 characters; a track text of 276 (the stock Ridge Valley's length: past the
        // U3 rewrite's frame), and of 4095 (the frame's 4 KB, full)
        for (int n : {276, 4095}) {
            fx_reset();
            strcpy((char*)w + 8, "uptown");
            *(int32_t*)(w + 0xca8) = 2;
            g_fx_xl_name = mk(0, 'n', 63);
            g_fx_xl_text = words(n);
            sprintf(m, "PreRaceDo, a friendly name of 63 characters and a track text of %d", n);
            fx_same(f, args, m);
        }
    }

    // ---- 6. AppProcessArgs: -d<dir> last, with no space after it ----
    {
        const Ent& f = fx_fn("AppProcessArgs");
        struct { const char* cmd; const char* dir; } cases[] = {{"-z -dC:\\games\\viper", "C:\\games\\viper"}, {"-d", ""}, {"/Ub -d../mods", "../mods"}};
        char* cmd = (char*)W.scratch;
        for (auto& c : cases) {
            fx_reset();
            memset(cmd, 0x5a, 0x200);
            strcpy(cmd, c.cmd);
            g_fx_dir[0] = 0;
            g_fx_dir_n = 0;
            UI_G8(S_Z_FLAG) = 0;
            UI_G32(S_APP_MODE) = 0;
            mem_save(g_snap);
            {
                FxPatch cd(0x00411ce0, (void*)&fx_FileChangeDir);
                r = fx_run(f, true, {U(cmd)});
            }
            sprintf(m, "AppProcessArgs \"%s\": a clean return", c.cmd);
            fx_check(fx_clean(f, r) && r.ret == 1, m, &r);
            sprintf(m, "AppProcessArgs \"%s\": the directory \"%s\" (\"%s\", %d changes)", c.cmd, c.dir, g_fx_dir, g_fx_dir_n);
            fx_check(g_fx_dir_n == 1 && !strcmp(g_fx_dir, c.dir), m);
            o = fx_outside(g_snap, {{(void*)(uintptr_t)S_Z_FLAG, 1}, {(void*)(uintptr_t)S_APP_MODE, 4}});
            sprintf(m, "AppProcessArgs \"%s\": nothing else written (the command line untouched)", c.cmd);
            fx_check(o == 0, m, 0, o);
        }
        fx_check(UI_G32(S_APP_MODE) == 1, "AppProcessArgs \"/Ub -d../mods\": -ub still read");
        fx_reset();
        memset(cmd, 0x5a, 0x200);
        strcpy(cmd, "-dfoo -z");
        fx_same(f, {U(cmd)}, "AppProcessArgs \"-dfoo -z\" (a space after the directory)");
    }

    // ---- 7. setup_blimp_jump / blimp_jump: -location's long words ----
    {
        const Ent& f = fx_fn("setup_blimp_jump");
        char* arg = (char*)W.scratch;
        // (base64.obj's alphabet: a-z, A-Z, 0-9, '#', '$': 32 'a's are a zero CompactFrame; kfr one at (1, 2, 3) turned 0.5
        // about z, in that alphabet)
        char kfr[33];
        {
            static const char al[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#$";
            const float cf[6] = {1.0f, 2.0f, 3.0f, 0.0f, 0.0f, 0.5f};
            uint8_t b[24];
            memcpy(b, cf, 24);
            for (int i = 0; i < 8; i++) {
                const uint32_t t = (uint32_t)b[3 * i] << 16 | (uint32_t)b[3 * i + 1] << 8 | b[3 * i + 2];
                for (int k = 0; k < 4; k++) kfr[4 * i + k] = al[(t >> (18 - 6 * k)) & 63];
            }
            kfr[32] = 0;
        }
        const std::string A32(32, 'a'), T40(40, 't'), T31(31, 't');
        // the frames the original makes of the two base64 frames
        uint32_t ref_id[12], ref_k[12];
        fx_reset();
        sprintf(arg, "( uptown %s )", A32.c_str());
        fx_run(f, false, {U(arg)});
        memcpy(ref_id, (const void*)(uintptr_t)S_BLIMP_START, 0x30);
        fx_reset();
        sprintf(arg, "( kenyon %s )", kfr);
        fx_run(f, false, {U(arg)});
        memcpy(ref_k, (const void*)(uintptr_t)S_BLIMP_START, 0x30);
        if (memcmp(ref_id, k_identity, 0x30) || getenv("VP_DEBUG_FIX")) {
            printf("  the zero frame:");
            for (int i = 0; i < 12; i++) printf(" %08x", ref_id[i]);
            printf("\n  the kenyon frame:");
            for (int i = 0; i < 12; i++) printf(" %08x", ref_k[i]);
            printf("\n");
        }
        fx_check(!memcmp(ref_id, k_identity, 0x30), "setup_blimp_jump: the zero frame is the identity (the test's premise)");
        struct { std::string a; std::string track; const uint32_t* frame; bool report; const char* what; } cases[] = {
            {"( " + T40 + " " + A32 + " )", T31, ref_id, false, "a 40-character track and a frame"},
            {"( uptown " + std::string(300, 'A') + " )", "uptown", k_identity, true, "a 300-character frame"},
            {"\t(\x0b" + T40 + "\r\n" + kfr + "\f)", T31, ref_k, false, "a 40-character track and a frame, other white space"},
            {"(" + std::string(300, 't'), T31, k_identity, true, "a 300-character track and no frame"},
        };
        for (auto& c : cases) {
            fx_reset();
            memset(arg, 0, 0x200);
            strcpy(arg, c.a.c_str());
            footprint(f, {U(arg)});
            mem_save(g_snap);
            r = fx_run(f, true, {U(arg)});
            sprintf(m, "setup_blimp_jump, %s: a clean return", c.what);
            fx_check(fx_clean(f, r), m, &r);
            o = fx_outside_fp(g_snap, fp);
            sprintf(m, "setup_blimp_jump, %s: nothing written outside the track's 0x20 bytes, the frame, base64's table", c.what);
            fx_check(o == 0, m, 0, o);
            sprintf(m, "setup_blimp_jump, %s: the track \"%s\"", c.what, c.track.c_str());
            fx_check(c.track == std::string((const char*)(uintptr_t)S_BLIMP_TRACK), m);
            sprintf(m, "setup_blimp_jump, %s: the frame%s", c.what, c.report ? " (the identity: not read), reported" : "");
            fx_check(!memcmp(c.frame, (const void*)(uintptr_t)S_BLIMP_START, 0x30) && fx_logged('LOGR') == c.report, m);
            if (getenv("VP_DEBUG_FIX")) {
                printf("  %s:", c.what);
                for (int i = 0; i < 12; i++) printf(" %08x", ((const uint32_t*)(uintptr_t)S_BLIMP_START)[i]);
                printf(" (logged %d)\n", (int)fx_logged('LOGR'));
            }
        }
        // the boundary: a 31-character track and a 255-character frame
        fx_reset();
        memset(arg, 0, 0x200);
        sprintf(arg, "( %s %s )", T31.c_str(), std::string(255, 'A').c_str());
        fx_same(f, {U(arg)}, "setup_blimp_jump, a 31-character track and a 255-character frame");
        // the word scan against the game's own sscanf, on random command lines (every white-space byte, '(' or not)
        {
            typedef int(__cdecl * Sscanf_t)(const char*, const char*, ...);
            static char b1[0x1000], b2[0x1000], a[0x400];
            fx_reset();
            int agree = 0, n = 0;
            for (int i = 0; i < 5000; i++) {
                int k = 0;
                const int parts = irange(0, 7);
                if (chance(80)) {
                    while (chance(40)) a[k++] = (char)irange(1, 0x20);
                    a[k++] = '(';
                }
                for (int p = 0; p < parts && k < 0x300; p++) {
                    const int ws = irange(0, 3);
                    for (int q = 0; q < ws; q++) a[k++] = (char)(chance(70) ? irange(9, 13) : chance(50) ? 0x20 : irange(1, 0x20));
                    const int wl = chance(70) ? irange(1, 12) : irange(1, 300);
                    for (int q = 0; q < wl && k < 0x3f0; q++) a[k++] = (char)(chance(90) ? irange(0x21, 0x7e) : irange(0x80, 0xff));
                }
                a[k] = 0;
                const char* w1;
                const char* w2;
                int32_t n1, n2;
                root_main::fix_scan_words(a, &w1, &n1, &w2, &n2);
                b1[0] = b2[0] = 0;
                const int ret = ((Sscanf_t)(uintptr_t)F_sscanf)(a, CP(0x004e3a58), b1, b2);
                const int got = (n1 >= 0) + (n2 >= 0);
                bool same = (ret < 0 ? 0 : ret) == got;
                if (same && n1 >= 0) same = (int32_t)strlen(b1) == n1 && !memcmp(b1, w1, (size_t)n1);
                if (same && n2 >= 0) same = (int32_t)strlen(b2) == n2 && !memcmp(b2, w2, (size_t)n2);
                n++;
                agree += same;
                if (!same && n - agree <= 3) printf("  the word scan and the game's sscanf disagree (%d / %d words) on \"%.60s\"\n", got, ret, a);
            }
            sprintf(m, "setup_blimp_jump's word scan agrees with the game's sscanf (%d of %d command lines)", agree, n);
            fx_check(agree == n, m);
            printf("setup_blimp_jump's word scan against the game's sscanf: %d random command lines, %d agree\n", n, agree);
        }
        // blimp_jump: the track copied into its World (a 40-character one, as the original setup_blimp_jump left it)
        {
            const Ent& b = fx_fn("blimp_jump");
            fx_reset();
            memset((void*)(uintptr_t)S_BLIMP_TRACK, 't', 40);
            UI_G8(S_BLIMP_TRACK + 40) = 0;
            memset(g_fx_world, 0, sizeof g_fx_world);
            {
                FxPatch lr(0x0040a840, (void*)&fx_LoadRace);
                r = fx_run(b, true, {});
            }
            fx_check(fx_clean(b, r), "blimp_jump, a 40-character track: a clean return", &r);
            fx_check(!memcmp(g_fx_world + 8, T31.c_str(), 31) && g_fx_world[8 + 31] == 0 && *(int32_t*)(g_fx_world + 0x28) == 0,
                     "blimp_jump, a 40-character track: the World's name its first 31 characters, its first car's type after it");
            fx_reset();
            strcpy((char*)(uintptr_t)S_BLIMP_TRACK, T31.c_str());
            fx_same(b, {}, "blimp_jump, a 31-character track");
        }
    }
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
    char* s = strstr(exe, "\\test\\world_root_race.cpp");
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
        {uit::F_LocaleConvertNumeric, (void*)&stub_LocaleConvertNumeric}, {F_tolower, (void*)&stub_tolower},
        {F_ControlToDisplayString, (void*)&stub_ControlToDisplayString}, {F_ControlUpdate, (void*)&stub_ControlUpdate},
        {F_ControlDetectInit, (void*)&stub_ControlDetectInit}, {F_ControlDetect, (void*)&stub_ControlDetect},
        {F_FileFindFirst, (void*)&stub_FindFirst}, {F_FileFindNext, (void*)&stub_FindNext},
        {F_FileFindClose, (void*)&stub_FindClose}, {F_FileOpen, (void*)&stub_FileOpen}, {F_FileClose, (void*)&stub_FileClose},
        {F_ClipboardGetText, (void*)&stub_ClipboardGetText},
        // this group's callees
        {0x004713e0, (void*)&stub_OptionsGet}, {0x004da350, (void*)&stub_stricmp}, {0x004da3e0, (void*)&stub_strnicmp},
        {0x0041b210, (void*)&stub_StringTableGet}, {0x0041b290, (void*)&stub_StringTableNumRows},
        {0x0041b2a0, (void*)&stub_StringTableGetEntry}, {0x00486aa0, (void*)&stub_MenuDoRaceMenu},
        {0x0040a3d0, (void*)&stub_GetGameState}, {0x004130e0, (void*)&stub_ScanDown}, {0x00462710, (void*)&stub_WorldGetPlayerCar},
        {0x00464480, (void*)&stub_CarMgrCount}, {0x00464490, (void*)&stub_CarMgrGetInfo}, {0x004627a0, (void*)&stub_WorldGameOptions},
        {0x0042a8b0, (void*)&stub_RecordGetRaceResult}, {0x0042a940, (void*)&stub_RecordGetLapResult},
        {0x0042bf30, (void*)&stub_PhysicsTimeString}, {0x00412430, (void*)&stub_get_user_directory},
        {0x00490690, (void*)&stub_BoardCreateControl}, {0x00422400, (void*)&stub_CenterLine_ctor},
        {0x00422490, (void*)&stub_CenterLine_dtor}, {0x00462070, (void*)&stub_WorldGetCarTexture},
        {0x00455900, (void*)&stub_mrModelLoadRemap}, {0x0044fd40, (void*)&stub_gxCanvasGet}, {0x00451550, (void*)&stub_gxPasteAlpha},
        {0x0044ee70, (void*)&stub_mrSetCamera}, {0x00455d00, (void*)&stub_mrModelDraw}, {0x0041ab50, (void*)&stub_LocaleMoney},
        {0x0041ac10, (void*)&stub_LocaleFormatShortDate},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    patch_gen(std::make_index_sequence<N_GEN>());
    struct { uint32_t slot; void* to; } iat[] = {
        {IAT_OpenClipboard, (void*)&stub_OpenClipboard}, {IAT_EmptyClipboard, (void*)&stub_EmptyClipboard},
        {IAT_GlobalAlloc, (void*)&stub_GlobalAlloc}, {IAT_GlobalLock, (void*)&stub_GlobalLock},
        {IAT_GlobalUnlock, (void*)&stub_GlobalUnlock}, {IAT_SetClipboardData, (void*)&stub_SetClipboardData},
        {IAT_CloseClipboard, (void*)&stub_CloseClipboard}, {IAT_IsWindow, (void*)&stub_IsWindow},
        {IAT_DestroyWindow, (void*)&stub_DestroyWindow},
    };
    for (auto& s2 : iat) *(void**)(uintptr_t)s2.slot = s2.to;
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    build_world();
    patch_jmp(0x004779a0, (void*)&stub_UIBegin);         // (after the world: it used the real ones)
    patch_jmp(0x00478380, (void*)&stub_UIEnd);
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
    long long both_fault = 0;
    int fixed_rounds = 0, fixed_bad = 0;                           // (the fix build: rounds kept out, a fixed case)
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0, fn_fpck = 0, fn_fixed = 0;
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
            const bool pre_fixed = ROOT_FIXES && fx_pre_case(f, words);
            const Result ro = run(f, false, words);
#if ROOT_FIXES
            if (pre_fixed || g_overran) {                           // a fixed case: the rewrite run alone, clean
                mem_load(g_snap);
                const Result rf = run(f, true, words);
                fixed_rounds++;
                fn_fixed++;
                if (rd == 0) { pure_n += fp.pure; replay_n += fp.replay_only != 0; }
                if (rf.fault) {
                    printf("  %08x %s: round %d, a fixed case: the rewrite faulted (%08x at %08x)\n", f.v10, f.name, rd, rf.code, rf.eip);
                    fixed_bad++;
                    fn_bad = true;
                }
                continue;
            }
#else
            (void)pre_fixed;
#endif
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
                    if (g_snap.data[i2] != DATA[i2] && !covered(DATA + i2)) out = 0x004e1000 + i2;
                for (uint32_t i2 = sizeof(HState); i2 < ARENA_BYTES && !out; i2++)
                    if (g_snap.arena[i2] != g_arena[i2] && !covered(g_arena + i2)) out = U(g_arena + i2);
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
#if ROOT_FIXES
            if (ro.fault && !rn.fault && !strcmp(f.name, "AppProcessArgs")) {   // a crash the fix removes
                fixed_rounds++;
                fn_fixed++;
                if (rd == 0) { pure_n += fp.pure; replay_n += fp.replay_only != 0; }
                continue;
            }
#endif
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
            printf("%08x %-52s %s%s (%d checks, %d with the footprint checked, %d faulted)\n", f.v10, f.name, fn_bad ? "BAD" : "ok",
                   fn_changed ? "" : " (never changed memory)", fn_checks, fn_fpck, fn_faults);
        if (fn_fixed) printf("  %08x %s: %d rounds reached a fixed case (kept out of the comparison; the rewrite clean in each)\n", f.v10,
                             f.name, fn_fixed);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    printf("%d functions (%d pure, %d replay_only in round 0, %d others), %d listed twice; %lld checks (%lld on poisoned .data), "
           "%lld logged words compared: %d differ, %d footprint violations, %d checks faulted (%lld in both, the same way); "
           "%d functions bad\n", g_nfns, pure_n, replay_n, g_nfns - pure_n - replay_n, dup, checks, poisoned_checks, log_words, differ,
           fp_bad, faults, both_fault, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    int fix_bad = 0;
#if ROOT_FIXES
    printf("the fix build: %d rounds reached a fixed case (kept out of the comparison), %d where the rewrite faulted\n", fixed_rounds,
           fixed_bad);
    if (!only) fix_bad = directed_fix_tests();
#else
    (void)fixed_rounds; (void)fixed_bad;
#endif
    return differ || fp_bad || dup || fix_bad || fixed_bad ? 1 : 0;
}
