// world_career.cpp -- M3 UI stage, step U4 (group A): every rewrite of hook/career_main.cpp (career.obj, season.obj),
// hook/career_screens.cpp (chooser.obj, events.obj, postseas.obj, ranking.obj, testing.obj) and the generated
// hook/career_leftover.cpp (the $E initialisers of libraries career, paintkit and intro, and group A's mechanical stubs,
// deleting destructors and destructor helpers) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_career.cpp
//        /Fo<dir>\ /Fe<dir>\world_career.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_career.exe [rounds] [seed]          (VP_TRACE=1: one line per function; VP_ONLY=text: those only;
//          VP_DEBUG_WORLD=1: the world and every fault's registers; VP_DEBUG_LOG=1: each round's first logged words)
//     (and with /DVP_CAREER_FIXES: the fix build, below)
//
// Loads out\race_v10.exe at 0x400000 the way test/world_root_race.cpp does (a child process with the range reserved) and
// includes the three files with PORT_FN redefined to list each function (its v1.0 address, the rewrite, its calling
// convention -- __cdecl, __thiscall as __fastcall --, stack arguments and return, its footprint). VP_FAITHFUL.
//
// The world. Once: the $E static initialisers of libraries ui, menu, root, career, paintkit and intro (their Xlators,
// colours, tables), the game's UIBegin (the styles); a locale; a CareerInfo, the chooser's eight, a season (the resource's
// layout: events with tracks, laps, purses, points), a driver lounge (its vtable's Get: a stub handing out fake drivers);
// then the career's controls by their own original constructors where they have one (CareerSummary, CareerBlurb,
// SeasonViewer, StandingsViewer) and written as the originals write them otherwise (CareerStatus, TrackImage,
// ClickThrough), all put in a full-screen window by the game's WidgetCreateWindow and _UIAddItems (CustomWidgets: adding
// one runs its control's Added -- CareerStatus adds its own items), then their Create. This is the pristine state.
//
// Then for EVERY function, `rounds` times (ten times that for one that doesn't run a dialog or a loop): pristine restored;
// half the rounds POISONED (.data/.bss random but for the strings and tables the code reads and what the ui library
// follows); the world's state random (the CareerInfo -- name ("TEST" now and then), class, week, next event, qualified,
// the qualifying times NaN / infinite / tiny, the driver map (a permutation, now and then not), the drivers' ranks, points
// and results, the awards, the funds, the seconds --, the chooser's slots, the season -- events 0..12, the purses and
// points, now and then a place out of range --, the statics the code reads, every function-local Xlator built or not,
// fresh or stale); arguments made for the function; for the loops and dialogs a random input script and random answers
// from the stubs (the career file's bytes -- valid, corrupt, short --, the pre-race screen's choice, the races' results,
// the dialog boxes' buttons, the options), ended by a watchdog (every dialog exits with -1 after 40 frames; the pre-race
// screen answers 0 after a few calls). The stack filled with one pattern after the FPU is set; the ORIGINAL runs from a
// raw call thunk; its results kept, the snapshot restored, the REWRITE runs. Compared: .data/.bss/.idata and the 1 MB arena
// (a pointer into this thread's stack in both counts as equal), the return value (st0 where there's one), the x87 depth,
// bytes popped, ebx / esi / edi / ebp, faults, and every stub's call log (the career file's writes by their bytes' hash,
// the Worlds handed to the races by their bytes'). Every byte the original changed must lie in the rewrite's footprint
// (unless replay_only); a pure one changes nothing but its output. The x87 at 24 or 53 bits.
//
// Stubbed (logged; their state in the arena so both passes see the same): world_root_race.cpp's set (memory, logs, input and
// time, the 2D calls, Xlator::xlate, sprintf, the options) and every callee outside the widget toolkit that starts, runs or
// ends a part of the game or touches a file: the files (open, create, append, read, write, remove, the user directory), the
// resources (the season), the races (LoadRace, UnloadRace, GenerateCarList, PreRaceDo, DoRace), the AI's driver map and
// track info, Random, ScanDown, the time of day, the locale's money and dates, Xlate / CouldXlate, the upgrade set, the
// upgrade shop and the credits (group B's), the dialog boxes (UIDoOkBox, UIDoYesNoBox, UIDoYesNoCancelBox), the tracks'
// names, GetRealismString, stricmp, the game's vsprintf, the hacks. The game's own code everywhere else: the widget toolkit
// (UIDoDialog, _UIAddItems, the groups, UIStyleDraw / Width / Height, UIAddDynamicStyle / UIRemoveStyle, UIStringList,
// UICustomControl's methods), UIDialogItem's, Xlator's, World's constructors, qsort, and every function of this group (each
// rewrite is checked against its original with the same callees). One harness choice: CareerStatus::Added, when the toolkit
// calls it through the vtable, zeroes the control's texts first (the screens add their stack CareerStatus before its texts
// are formatted -- stack garbage, which the rewrites' frames can't reproduce; see hook_status_added). VP_COVER=1 prints,
// per modal function, how often each round reached the deep callees (the races, the career file, the dialog boxes).
//
// Built with /DVP_CAREER_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Career").
// Every function is still compared as above, with the rounds that reach a fixed case kept out of the comparison and
// counted (per function, listed), the rewrite still run on each and required to return cleanly (or to fault just where
// the original does, on the round's own damage -- a null season drawn under the dialog, say): create_cb's where the
// default name's translation is over 15 characters (the stubs' "A longer translation": the original leaves the name
// unterminated when no player_name is saved). No other fixed case is in the random world's reach (user directories of
// 2..60 characters, translations, track and driver names of 20 at most). Then
// directed_fix_tests: for each fix, the bad case run on the rewrite alone -- no fault, the bytes popped and ebx / esi /
// edi / ebp kept, nothing written outside what it may write (the memory around it compared; for the dialogs, what the
// frame held after the name), and the result the fix promises -- and its boundary case (the longest input that fits) on
// both, compared bit for bit. Without it (VP_FAITHFUL) every rewrite must match its original bit for bit.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>
#include <tuple>
#include <type_traits>
#include <utility>
#include <string>
#include <vector>

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

#include "../hook/career_main.cpp"
#include "../hook/career_screens.cpp"
#include "../hook/career_leftover.cpp"

using namespace uit;
using namespace car;

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
    uint32_t rs;                                  // the stubs' random answers
    uint32_t scan_mask[8];                        // ScanDown's keys held (256 bits)
    int32_t prerace_calls, prerace_end;           // PreRaceDo: 0 (leave) from prerace_end on
    uint32_t opt_mask;
    int32_t opt_val[8];
    int32_t userdir_len;
    int32_t file_open_fail, file_create_fail;     // percent
    int32_t file_pos;                             // the open file's read position
    int32_t file_img;                             // which image it reads
    int32_t file_len[2];                          // the images' lengths (short: a truncated file)
    uint8_t file[2][0x690];                       // two career files: the 16-byte header and the xor'd body
    int32_t res_fail;                             // ResourceGet answers 0 (percent)
    int32_t box_answer;                           // the dialog boxes: -1 random
    int32_t track_count;
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
enum { LOG_MAX = 1 << 17 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void L(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static uintptr_t g_self_lo, g_self_hi;
// (64-bit arithmetic: an address near the top plus a length doesn't wrap into range)
static bool readable(const void* p, uint32_t n) {
    const uint64_t a = (uint64_t)(uintptr_t)p, e = a + (uint64_t)n;
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
static uint32_t hash_bytes(const void* p, uint32_t n) {
    uint32_t h = 2166136261u;
    if (!readable(p, n ? n : 1)) return 0xbadbad;
    for (uint32_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static uint32_t hash_str(const char* s) {
    uint32_t h = 2166136261u;
    if (!s || !readable(s, 1)) return 0x5eed;
    for (; readable(s, 1) && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static void L_canvas() {
    const gxCanvas* c = UI_GP(gxCanvas, S_GX_CANVAS);
    L(P(c));
    if (c && readable(c, sizeof(gxCanvas))) { L((uint32_t)c->cx0); L((uint32_t)c->cy0); L((uint32_t)c->cx1); L((uint32_t)c->cy1); }
}
// a World handed to a stub: its bytes by hash, its track and count
static void L_world(const void* w) {
    L(P(w));
    if (!readable(w, 0xcd4)) { L('BADW'); return; }
    L(hash_bytes(w, 0xcd4));
    LS((const char*)w + 8, 0x20);
    L(*(const uint32_t*)((const uint8_t*)w + 0xca8));
}

// ---- stubs: memory, logs, sync (world_root_race.cpp's) --------------------------------------------------------------------------
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
// (the watchdog: past the round's frame limit every dialog is told to exit with -1, which ends every loop of the career's)
static int32_t __cdecl stub_PTimeNow() {
    L('TIME');
    HS->time += 16;
    HS->frames++;
    if (HS->frames > HS->frame_limit) { UI_G8(S_EXIT) = 1; UI_G32(S_EXIT_CODE) = -1; L('WDOG'); }
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

// ---- stubs: the 2D calls (world_root_race.cpp's) ------------------------------------------------------------------------------
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
static void __cdecl stub_gxFlip() { L('FLIP'); }
static void __cdecl stub_mrBeginFrame() { L('MRBF'); }
static void __cdecl stub_mrEndFrame() { L('MREF'); }

// ---- stubs: text, controls (world_root_race.cpp's) -------------------------------------------------------------------------------
static const char* const k_xl_text[16] = {"s", "bhp", "rpm", "Week", "Season", "Laps", "Pack", "Pro", "Club", "DNF", "Best",
                                          "Car", "Track", "Weather", "Time of day", "A longer translation"};
static uint32_t g_xl_seed;
static const char* xl_text(uint32_t key) {
    // (a key on this thread's stack -- a poisoned one -- differs between the passes: not read)
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
static void __cdecl stub_ControlToDisplayString(char* buf, int n, const uint32_t* ctl) {
    L('CTDS'); L(P(buf)); L((uint32_t)n); L(P(ctl));
    if (n > 0) { strncpy(buf, "ctl", (size_t)n - 1); buf[n - 1] = 0; }
}
static void __cdecl stub_ControlUpdate(uint8_t u) { L('CUPD'); L(u); }
static void __cdecl stub_ControlDetectInit() { L('CDIN'); }
static uint8_t __cdecl stub_ControlDetect(uint32_t* ctl) { L('CDET'); L(P(ctl)); return 0; }
static uint8_t __cdecl stub_ClipboardGetText(char* buf, int n) { L('CLIP'); if (n > 0) buf[0] = 0; return 0; }
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    L('SPRF'); L(P(buf)); LS(fmt);
    va_list ap;
    va_start(ap, fmt);
    L_va(fmt, (const uint32_t*)ap);
    const int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    LS(buf);
    return r;
}
static int __cdecl stub_vsprintf(char* buf, const char* fmt, va_list ap) {
    L('VSPF'); L(P(buf)); LS(fmt);
    L_va(fmt, (const uint32_t*)ap);
    const int r = vsprintf(buf, fmt, ap);
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

// ---- stubs: the options --------------------------------------------------------------------------------------------------------
static void __cdecl stub_OptionsGetI(const char* sec, const char* key, int32_t* out) {
    L('OPTI'); LS(sec); LS(key); L(P(out));
    const uint32_t h = hash_str(key) ^ HS->opt_mask;
    if (h & 1) { *out = HS->opt_val[(h >> 1) & 7]; L((uint32_t)*out); }
}
static void __cdecl stub_OptionsGetB(const char* sec, const char* key, uint8_t* out) {
    L('OPTB'); LS(sec); LS(key); L(P(out));
    const uint32_t h = hash_str(key) ^ HS->opt_mask;
    if (h & 1) { *out = (uint8_t)HS->opt_val[(h >> 1) & 7]; L(*out); }
}
static void __cdecl stub_OptionsGetS(const char* sec, const char* key, char* out, int32_t n) {
    L('OPTS'); LS(sec); LS(key); L(P(out)); L((uint32_t)n);
    const uint32_t h = hash_str(key) ^ HS->opt_mask;
    if ((h & 3) && n > 0) {
        static const char* const names[4] = {"Ann", "Bob Jones", "TEST", "Zelda Zipperton"};
        _snprintf(out, (size_t)n, "%s", names[(h >> 2) & 3]);
        out[n - 1] = 0;
        LS(out);
    }
}
static void __cdecl stub_OptionsSetI(const char* sec, const char* key, int32_t v) { L('OPTW'); LS(sec); LS(key); L((uint32_t)v); }

// ---- stubs: the files (the career files as two images, the log) ------------------------------------------------------------------
static int32_t __cdecl stub_FileOpen(const char* name) {
    L('FOPN'); LS(name);
    if ((int32_t)(hs_rand() % 100) < HS->file_open_fail) return 0;
    HS->file_pos = 0;
    HS->file_img = (int32_t)(hash_str(name) & 1);
    return 0x0f11e0;
}
static int32_t __cdecl stub_FileCreate(const char* name) { L('FCRT'); LS(name); return (int32_t)(hs_rand() % 100) < HS->file_create_fail ? 0 : 0x0f11e1; }
static int32_t __cdecl stub_FileAppend(const char* name) { L('FAPP'); LS(name); return (hs_rand() & 1) ? 0x0f11e2 : 0; }
static void __cdecl stub_FileClose(int32_t* fd) { L('FCLS'); L((uint32_t)*fd); *fd = 0; }
static uint8_t __cdecl stub_FileReadExact(int32_t h, void* buf, int32_t n) {
    L('FRDX'); L((uint32_t)h); L(P(buf)); L((uint32_t)n);
    const int32_t img = HS->file_img & 1;
    if (n < 0 || HS->file_pos + n > HS->file_len[img]) { L('SHRT'); return 0; }
    memcpy(buf, HS->file[img] + HS->file_pos, (size_t)n);
    HS->file_pos += n;
    return 1;
}
static uint8_t __cdecl stub_FileWrite(int32_t h, const void* p, int32_t n) {
    L('FWRT'); L((uint32_t)h); L((uint32_t)n);
    L(n >= 0 && n <= 0x10000 ? hash_bytes(p, (uint32_t)n) : 'NBAD');
    return 1;
}
static uint8_t __cdecl stub_FileRemove(const char* name) { L('FREM'); LS(name); return 1; }
static uint8_t __cdecl stub_FileCreateDirectory(const char* name) { L('FMKD'); LS(name); return 1; }
static char g_userdir[0x100];
static const char* __cdecl stub_Win32GetUserDirectory() {
    L('UDIR');
    const int32_t n = HS->userdir_len;
    for (int32_t i = 0; i < n && i < 0xff; i++) g_userdir[i] = (char)('a' + i % 26);
    g_userdir[n > 0 && n < 0xff ? n : 0] = 0;
    if (n > 0 && n < 0xff) g_userdir[n - 1] = '\\';
    return g_userdir;
}

// ---- stubs: the career's other callees -----------------------------------------------------------------------------------------
static uint8_t __cdecl stub_ScanDown(uint32_t k) { L('SCND'); L(k & 0xff); return (uint8_t)((HS->scan_mask[(k >> 5) & 7] >> (k & 31)) & 1); }
static void __cdecl stub_TimeGetTimeOfDay(uint16_t* t) {
    L('TOD '); L(P(t));
    for (int i = 0; i < 4; i++) t[i] = (uint16_t)hs_rand();
}
static int32_t __cdecl stub_Random(int32_t n) { L('RAND'); L((uint32_t)n); const int32_t r = n > 0 ? (int32_t)(hs_rand() % (uint32_t)n) : 0; L((uint32_t)r); return r; }
static uint8_t* g_season;                         // the fake season (arena)
static void* __cdecl stub_ResourceGet(const char* name, uint32_t type, uint32_t* size, int32_t* x, uint8_t* a, uint8_t* b) {
    L('RGET'); LS(name); L(type); L(P(size)); L(P(x)); L(P(a)); L(P(b));
    if ((int32_t)(hs_rand() % 100) < HS->res_fail) return 0;
    if (size) *size = 0x24 + 12 * 0x58;
    return g_season;
}
static uint8_t __cdecl stub_ResourceForget(void* p) { L('RFGT'); L(P(p)); return 1; }
static const char* __cdecl stub_Xlate(const char* s) { L('XLTE'); LS(s); return xl_text((uint32_t)(uintptr_t)s); }
static uint8_t __cdecl stub_CouldXlate(const char* s) { L('CXLT'); LS(s); return (uint8_t)((hs_rand() % 16) != 0); }
static void __cdecl stub_AISetDriverMap(const int32_t* m, int32_t n) {
    L('AISM'); L(P(m)); L((uint32_t)n);
    if (readable(m, 32)) for (int i = 0; i < 8; i++) L((uint32_t)m[i]);
}
static uint8_t* g_trackinfo;                      // 16 x 0x20 (arena)
static const void* __cdecl stub_AIGetTrackInfo(int32_t t) { L('AGTI'); L((uint32_t)t); return g_trackinfo + ((uint32_t)t & 15) * 0x20u; }
static const char* const k_tracks[] = {"bemidji", "heaven", "uptown", "hastings", "limbo", "dundas", "nfield", "kenyon", "hell"};
static const char* __cdecl stub_GetTrackName(int32_t i) { L('GTRN'); L((uint32_t)i); return k_tracks[(uint32_t)i % 9]; }
static const char* __cdecl stub_GetTrackFriendlyName(int32_t i) { L('GTRF'); L((uint32_t)i); return xl_text(0x1000u + (uint32_t)i); }
static int32_t __cdecl stub_GetTrackNumber(const char* s) {
    L('GTNM'); LS(s);
    for (int i = 0; i < HS->track_count && i < 9; i++) if (readable(s, 1) && !_stricmp(s, k_tracks[i])) return i;
    return -1;
}
static const char* __cdecl stub_GetRealismString(int32_t r) { L('GRLS'); L((uint32_t)r); static const char* const k[3] = {"Arcade", "Normal", "Real"}; return k[(uint32_t)r % 3]; }
static void __cdecl stub_LoadRace(void* w) { L('LDRC'); L_world(w); }
static void __cdecl stub_UnloadRace(const void* w) { L('ULRC'); L_world(w); }
static void __cdecl stub_GenerateCarList(uint8_t* w, const char* car, int32_t player) {
    L('GCRL'); L_world(w); LS(car); L((uint32_t)player);
    if (!readable(w, 0xcd4)) return;
    const int32_t n = (hs_rand() & 7) == 0 ? 1 + (int32_t)(hs_rand() % 8) : 8;
    *(int32_t*)(w + 0xca8) = n;
    for (int i = 0; i < 16; i++) {
        uint8_t* e = w + 0x28 + i * 0xc8;
        *(int32_t*)e = (i == player || (hs_rand() & 15) == 0) ? 0 : 1;
        sprintf((char*)e + 4, "Drv%d", i);
        strcpy((char*)e + 0x11, "viper");
        *(int32_t*)(e + 0xc0) = i;
    }
}
static int32_t __cdecl stub_PreRaceDo(uint8_t* w, uint32_t garage, uint32_t car, uint32_t qual, uint32_t times, uint32_t prizes) {
    L('PRDO'); L_world(w); L(garage); L(P((void*)(uintptr_t)car)); L(qual); L(P((void*)(uintptr_t)times));
    if (readable((void*)(uintptr_t)times, 32)) L(hash_bytes((void*)(uintptr_t)times, 32));
    L(P((void*)(uintptr_t)prizes));
    if (readable((void*)(uintptr_t)prizes, 64)) L(hash_bytes((void*)(uintptr_t)prizes, 64));
    const int32_t n = ++HS->prerace_calls;
    const int32_t r = n >= HS->prerace_end ? 0 : (int32_t)(hs_rand() % 4);
    L((uint32_t)r);
    return r;
}
// DoRace: the World logged, the Results filled (the places a permutation of 1..8 now and then broken, the player's time)
static void __cdecl stub_DoRace(uint8_t* w, uint8_t* res, void(__cdecl* post)(void*)) {
    L('DORC'); L_world(w); L(P(res)); L((uint32_t)(uintptr_t)post);
    if (res && readable(res, 0xc4)) {
        int32_t pl[16];
        for (int i = 0; i < 16; i++) pl[i] = i + 1;
        for (int i = 7; i > 0; i--) { const int j = (int)(hs_rand() % (uint32_t)(i + 1)); const int32_t t = pl[i]; pl[i] = pl[j]; pl[j] = t; }
        if ((hs_rand() & 7) == 0) pl[hs_rand() % 8] = (int32_t)(hs_rand() % 12) - 1;
        for (int i = 0; i < 16; i++) *(int32_t*)(res + 4 + i * 12) = pl[i];
        const uint32_t t = (hs_rand() & 7) == 0 ? 0x7fc00000u : 0x42000000u + (hs_rand() & 0xfffff);
        memcpy(res + 0xc, &t, 4);
    }
    if (post) post(w);
}
static void* __cdecl stub_CarFileGetUpgradeSet(const char* n) { L('GUGS'); LS(n); return (void*)(uintptr_t)0x00ab0000; }
static void __cdecl stub_CarFileForgetUpgradeSet(void* p) { L('FUGS'); L(P(p)); }
static int32_t box_answer(int32_t a, int32_t b, int32_t c) {
    const uint32_t r = hs_rand() % 3;
    return r == 0 ? a : r == 1 ? b : c;
}
static void __cdecl stub_UIDoOkBox(const char* t, const char* m) { L('OKBX'); LS(t); LS(m); }
static uint8_t __cdecl stub_UIDoYesNoBox(const char* t, const char* m, void* idle) { L('YNBX'); LS(t); LS(m); L(P(idle)); const uint8_t r = (uint8_t)(hs_rand() & 1); L(r); return r; }
static int32_t __cdecl stub_UIDoYesNoCancelBox(const char* t, const char* m) { L('YNCB'); LS(t); LS(m); const int32_t r = box_answer(-3, -2, -1); L((uint32_t)r); return r; }
// the driver lounge: Get(strength, driver) hands out one of 8 fake drivers (their per-track skills, a name at +0x210)
static uint8_t* g_drivers;                        // 8 x 0x220 (arena)
static uint32_t g_lounge_vt[4];
static uint32_t g_lounge_obj[4];
static const uint8_t* __fastcall stub_lounge_get(void* self, int, int32_t s, int32_t i) {
    L('LGET'); L(P(self)); L((uint32_t)s); L((uint32_t)i);
    return g_drivers + ((uint32_t)i & 7) * 0x220u;
}
static void __fastcall stub_lounge_other(void* self, int) { L('LOTH'); L(P(self)); }

// ---- the generic stubs: logged arguments (d a dword -- a pointer into the stack as one word --, b its low byte, s a string),
// a random answer from the arena's stream (v none, b mostly 0, B mostly 1, i 0..3) ------------------------------------------------
// (world_root_race.cpp's list, less what's stubbed above or run for real here -- CareerDo --, plus this group's)
#define GEN_STUBS(X)                                                                                                          \
    X(0x00419560, "", 'v') X(0x00419590, "", 'v') X(0x004cc600, "", 'v') X(0x0044deb0, "", 'v') X(0x0044e7e0, "", 'v')     \
    X(0x0044df20, "d", 'v') X(0x004cccd0, "s", 'v') X(0x00470ee0, "", 'v') X(0x0041a7f0, "", 'v') X(0x00471cb0, "", 'B')   \
    X(0x00419a30, "s", 'v') X(0x0040d380, "b", 'v') X(0x004a1670, "", 'v') X(0x00420bd0, "", 'v')                          \
    X(0x00412d80, "", 'v') X(0x00420c00, "", 'v') X(0x004a1680, "", 'v') X(0x0040d4a0, "", 'v') X(0x00419bb0, "s", 'v')    \
    X(0x00471d90, "", 'v') X(0x0044e000, "", 'v') X(0x0044e800, "", 'v') X(0x0044dee0, "", 'v') X(0x0041ab00, "", 'v')     \
    X(0x00471330, "", 'v') X(0x0044e1b0, "", 'v') X(0x00411ce0, "s", 'B') X(0x0040a680, "", 'v') X(0x0040a7e0, "", 'v')    \
    X(0x004819a0, "", 'v') X(0x00406bc0, "d", 'v') X(0x004b0a30, "", 'v') X(0x0041f050, "d", 'b')                          \
    X(0x00471110, "", 'v') X(0x00413770, "", 'v') X(0x0040ac90, "", 'v') X(0x004719a0, "", 'v')                            \
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
    X(0x004a2a80, "", 'b') X(0x004123b0, "", 'B') X(0x004123a0, "", 'B')                                                   \
    X(0x00418620, "", 'B') X(0x004a36a0, "w", 'v') X(0x00418710, "", 'v') X(0x00412490, "", 'v') X(0x00418340, "", 'B')    \
    X(0x00418460, "", 'v') X(0x00412420, "", 'v') X(0x0041b270, "d", 'v') X(0x004911a0, "ssds", 'B')                       \
    X(0x004654b0, "dss", 'B') X(0x0040cbc0, "", 'v') X(0x00455950, "d", 'v') X(0x0044fdc0, "d", 'v')                       \
    X(0x0044ecd0, "ddddb", 'v') X(0x0044ede0, "ddd", 'v') X(0x00490740, "sddb", 'v') X(0x00462760, "", 'i')                \
    X(0x0040d350, "", 'v') X(0x0040d370, "", 'v') X(0x00420c30, "", 'v') X(0x004c29b0, "", 'v') X(0x004c5660, "d", 'v')    \
    X(0x00412bf0, "", 'v')
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
template <size_t... I> static void patch_gen(std::index_sequence<I...>) { (patch_jmp(g_gen[I].at, (void*)&gen_stub<(int)I>), ...); }
// CareerStatus::Added through the vtable (the toolkit's call when a screen's items are added): the control's three texts
// zeroed first. The screens put their CareerStatus on the stack and add it before its Callback formats the texts, so the
// StaticTexts it adds copy whatever the stack held (the original's garbage: return addresses and locals of the calls made
// before, at depths the rewrite's frames can't share -- a FIX CANDIDATE, marked in place). Zero is one value that garbage
// can take; with it both passes read the same. (CareerStatus::Added's own rounds call it directly, texts random.)
static uint32_t g_status_added_orig;
static void __fastcall hook_status_added(uint8_t* self, int) {
    memset(self + 0x18, 0, 0xc0);
    ((void(__fastcall*)(void*, int))(uintptr_t)g_status_added_orig)(self, 0);
}
static void __cdecl stub_UIBegin() { L('UIBG'); }
static void __cdecl stub_UIEnd() { L('UIEN'); }

// ---- the raw call (world_root_race.cpp's) --------------------------------------------------------------------------------------
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
// the stack the call will use, filled with one pattern, after the FPU is set (world_root_race.cpp's)
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
    static const char* const libs[] = {"ui", "menu", "root", "career", "paintkit", "intro"};
    while (fgets(line, sizeof line, f)) {
        char* c[6] = {line};
        int k = 1;
        for (char* p = line; *p && k < 6; p++)
            if (*p == ',') { *p = 0; c[k++] = p + 1; }
        if (k < 6) continue;
        bool lib = false;
        for (const char* l : libs) lib |= !strcmp(c[2], l);
        if (lib && c[5][0] == '$' && c[5][1] == 'E') g_setup_e.push_back((uint32_t)strtoul(c[0], 0, 16));
    }
    fclose(f);
}

struct World_ {
    gxCanvas* screen;
    gxCanvas* canvas2;
    WidgetWindow* win;
    CareerStatus* status;
    CareerSummary* summary;
    CareerSummary* blurb;
    TwoStyles* season_view;
    TwoStyles* standings;
    TrackImage* track_image;
    uit::UICustomControl* click;
    uint8_t* world;                               // a World (0xcd4), for sort_carlist / GenerateCarList / null_postrace
    uint8_t* results;                             // Results (0xc4)
    uint8_t* opts;                                // GameOptions (0x28)
    uint8_t* info_buf;                            // a CareerInfo-sized buffer (the file functions)
    uint8_t* block;                               // checksum / xor_block's (0x800)
    int32_t* ints;                                // driver_compare's operands, a map copy
    char* texts[8];
    char* track_buf;                              // testing_menu's out (0x40)
    uint8_t* flags;
    uint8_t* scratch;                             // for constructors and deleting destructors (0x200)
    std::vector<Widget*> widgets;
};
static World_ W;

// a career: a name, the class, the season's progress, the drivers
static const char* const k_names[] = {"Ann", "Bob Jones", "TEST", "Zelda Zipperton", "x", "Viper Pilot"};
static void random_info(uint8_t* ci, bool plausible) {
    memset(ci, 0, S_INFO_SIZE);
    if (!plausible) { for (uint32_t i = 0; i < S_INFO_SIZE; i++) ci[i] = (uint8_t)rnd(); ci[0x0f] = 0; return; }
    strcpy((char*)ci, chance(15) ? "TEST" : k_names[rnd() % 6]);
    if (chance(5)) ci[0] = 0;
    *(int32_t*)(ci + 0x10) = irange(0, 2);
    ci[0x14] = (uint8_t)(rnd() & 1);
    *(int32_t*)(ci + 0x18) = chance(10) ? (int32_t)rnd() : irange(0, 200000);
    for (int i = 0; i < 0x100; i++) ci[0x1c + i] = (uint8_t)(chance(70) ? 0 : rnd());
    *(int32_t*)(ci + 0x11c) = irange(0, 9);
    *(int32_t*)(ci + 0x120) = irange(0, 30);
    *(int32_t*)(ci + 0x124) = chance(95) ? irange(0, 3) : irange(0, 4);
    *(int32_t*)(ci + 0x128) = irange(0, 12);
    ci[0x12c] = (uint8_t)(chance(50) ? 1 : 0);
    for (int i = 0; i < 8; i++) *(uint32_t*)(ci + 0x130 + 4 * i) = fbits(fval(20.0f, 120.0f));
    int32_t perm[8];
    for (int i = 0; i < 8; i++) perm[i] = i;
    for (int i = 7; i > 0; i--) { const int j = irange(0, i); const int32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
    if (chance(5)) perm[rnd() % 8] = perm[rnd() % 8];         // a player twice, or none
    for (int i = 0; i < 8; i++) *(int32_t*)(ci + 0x150 + 4 * i) = perm[i];
    for (int d = 0; d < 8; d++) {
        uint8_t* r = ci + 0x170 + 0x88 * d;
        *(int32_t*)r = irange(0, 7);
        *(int32_t*)(r + 4) = chance(30) ? irange(0, 3) : irange(0, 200);
        for (int k = 0; k < 32; k++) *(int32_t*)(r + 8 + 4 * k) = chance(60) ? 0 : chance(90) ? irange(0, 8) : irange(-2, 12);
    }
    for (int i = 0; i < 32; i++) *(int32_t*)(ci + 0x5b0 + 4 * i) = chance(60) ? 0 : irange(1, 5);
    *(int32_t*)(ci + 0x630) = chance(10) ? (int32_t)rnd() : irange(0, 400000);
}
// the season: events on the tracks, their laps, purses (by place 1..8) and points
static void random_season(uint8_t* s) {
    memset(s, 0, 0x24 + 12 * 0x58);
    *(int32_t*)s = chance(90) ? irange(1, 12) : irange(0, 12);
    for (int i = 0; i < 8; i++) *(int32_t*)(s + 4 + 4 * i) = chance(10) ? (int32_t)rnd() : irange(0, 50000);
    for (int e = 0; e < 12; e++) {
        uint8_t* ev = s + 0x24 + 0x58 * e;
        strcpy((char*)ev, chance(95) ? k_tracks[rnd() % 9] : "nowhere");
        ev[0x10] = (uint8_t)(rnd() & 1);
        *(int32_t*)(ev + 0x14) = irange(1, 20);
        for (int k = 0; k < 8; k++) *(int32_t*)(ev + 0x18 + 4 * k) = irange(0, 30000);
        for (int k = 0; k < 8; k++) *(int32_t*)(ev + 0x38 + 4 * k) = irange(0, 12);
    }
}
// a career file's image: the header and the body xor'd; now and then corrupt or short
static void random_file(int i) {
    uint8_t body[S_INFO_SIZE];
    random_info(body, chance(90));
    uint32_t sum = 0;
    for (uint32_t k = 0; k < S_INFO_SIZE; k++) sum += body[k];
    uint32_t hdr[4] = {0x43494e46u, 2, S_INFO_SIZE, sum};
    if (chance(8)) hdr[0] ^= 1u << (rnd() % 32);
    if (chance(5)) hdr[1] = rnd() % 4;
    if (chance(5)) hdr[2] = rnd() % 0x800;
    if (chance(8)) hdr[3] += 1 + rnd() % 5;
    memcpy(HS->file[i], hdr, 16);
    for (uint32_t k = 0; k < S_INFO_SIZE; k++) HS->file[i][16 + k] = (uint8_t)(body[k] ^ 0xa4);
    HS->file_len[i] = chance(90) ? 16 + (int32_t)S_INFO_SIZE : irange(0, 16 + (int32_t)S_INFO_SIZE);
}

static void build_world() {
    g_wv = sizeof(HState) + 0x100;
    HS->heap_next = A_HEAP;
    HS->frame_limit = 1 << 30;
    HS->rs = 0x12345;
    HS->prerace_end = 1 << 30;
    HS->track_count = 9;
    HS->userdir_len = 20;
    UI_G32(S_SCREEN_W) = 640;
    UI_G32(S_SCREEN_H) = 480;
    g_season = (uint8_t*)wv(0x24 + 12 * 0x58);
    g_trackinfo = (uint8_t*)wv(16 * 0x20);
    g_drivers = (uint8_t*)wv(8 * 0x220);
    for (int i = 0; i < 8; i++) {
        uint8_t* d = g_drivers + i * 0x220;
        char* name = wstr(k_names[i % 6]);
        *(uint32_t*)(d + 0x210) = U(name);
    }
    g_lounge_vt[0] = U(&stub_lounge_other); g_lounge_vt[1] = U(&stub_lounge_get); g_lounge_vt[2] = U(&stub_lounge_other);
    g_lounge_vt[3] = U(&stub_lounge_other);
    g_lounge_obj[0] = U(g_lounge_vt);
    for (uint32_t a : g_setup_e) call_orig(a, 0, {});
    call_orig(0x004779a0, 0, {});                      // UIBegin
    W.screen = (gxCanvas*)wv(sizeof(gxCanvas));
    W.screen->format = 4; W.screen->pixels = (uint8_t*)0x7b000000; W.screen->w = 640; W.screen->h = 480; W.screen->pitch = 1280;
    W.screen->cx1 = 640; W.screen->cy1 = 480;
    W.canvas2 = (gxCanvas*)wv(sizeof(gxCanvas));
    memcpy((void*)W.canvas2, (const void*)W.screen, sizeof(gxCanvas));
    W.canvas2->w = 320; W.canvas2->h = 200; W.canvas2->cx0 = 10; W.canvas2->cy0 = 5; W.canvas2->cx1 = 300; W.canvas2->cy1 = 190;
    UI_GP(gxCanvas, S_GX_CANVAS) = W.screen;
    UI_GU32(S_LOUNGE) = U(g_lounge_obj);
    random_info((uint8_t*)(uintptr_t)S_INFO, true);
    for (int s = 0; s < 8; s++) random_info((uint8_t*)(uintptr_t)(S_CH_INFOS + S_INFO_SIZE * (uint32_t)s), true);
    random_season(g_season);
    UI_GP(uint8_t, S_SEASON) = g_season;
    W.world = (uint8_t*)wv(0xcd4);
    W.results = (uint8_t*)wv(0xc4);
    W.opts = (uint8_t*)wv(0x28);
    W.info_buf = (uint8_t*)wv(S_INFO_SIZE + 0x10);
    W.block = (uint8_t*)wv(0x800);
    W.ints = (int32_t*)wv(0x40);
    for (int i = 0; i < 8; i++) W.texts[i] = (char*)wv(0x40);
    W.track_buf = (char*)wv(0x40);
    W.flags = (uint8_t*)wv(16);
    W.scratch = (uint8_t*)wv(0x200);
    // the controls: CareerStatus written as the screens write theirs, the others by their constructors
    W.status = (CareerStatus*)wv(sizeof(CareerStatus));
    W.status->vtbl = (const void*)(uintptr_t)VT_CareerStatus;
    W.summary = (CareerSummary*)wv(sizeof(CareerSummary));
    call_orig(F_CareerSummary_ctor, 1, {U(W.summary), 0, S_INFO});
    W.blurb = (CareerSummary*)wv(sizeof(CareerSummary));
    call_orig(F_CareerSummary_ctor, 1, {U(W.blurb), 0, S_CH_INFOS});
    W.blurb->vtbl = (const void*)(uintptr_t)VT_CareerBlurb;
    W.season_view = (TwoStyles*)wv(sizeof(TwoStyles));
    call_orig(F_SeasonViewer_ctor, 1, {U(W.season_view), 0});
    W.standings = (TwoStyles*)wv(sizeof(TwoStyles));
    call_orig(F_StandingsViewer_ctor, 1, {U(W.standings), 0});
    W.track_image = (TrackImage*)wv(sizeof(TrackImage));
    W.track_image->vtbl = (const void*)(uintptr_t)VT_TrackImage;
    W.track_image->track = (const int32_t*)(uintptr_t)S_TS_TRACK;
    W.click = (uit::UICustomControl*)wv(sizeof(uit::UICustomControl));
    W.click->vtbl = (const void*)(uintptr_t)VT_ClickThrough;
    // the chooser's list (refresh's) and groups
    {
        void* m = wv(0x14);
        call_orig(uit::F_UIStringList_ctor, 1, {U(m), 0, 8, 0xe});
        UI_GP(void, S_CH_LIST) = m;
    }
    // a full-screen window with each control in a CustomWidget
    UIDialogItem* items = (UIDialogItem*)wv(sizeof(UIDialogItem) * 12);
    UIDialogItem* it = items;
    it = it_(it, 23, 0, 0, 0, 0x1ea, 0x1d0, "", 0, W.status, 0);
    it = it_(it, 23, 0, 0xbb, 0xc1, 0xa5, 0x86, "", 0, W.summary, 0);
    it = it_(it, 23, 0, 0x139, 0xc1, 0xa5, 0x86, "", 0, W.blurb, 0);
    it = it_(it, 23, 0, 0x32, 0x78, 0x208, 0x122, "", 0, W.season_view, 0);
    it = it_(it, 23, 0, 0x32, 0x78, 0x208, 0x122, "", 0, W.standings, 0);
    it = it_(it, 23, 0, 0x190, 0x96, 0x96, 0x96, "", 0, W.track_image, 0);
    it = it_(it, 23, 0, 0, 0, 640, 480, "", 0, W.click, 0);
    it = it_(it, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    call_orig(F_WidgetCreateWindow, 0, {0, 0, 0, 640, 480, 2});
    W.win = (WidgetWindow*)(uintptr_t)g_setup_ret;
    UI_GP(WidgetWindow, S_ACTIVE) = W.win;
    call_orig(F_UIAddItems, 0, {U(W.win), U(items)});
    const void* ctl[] = {W.status, W.blurb, W.track_image};
    for (const void* c : ctl) call_orig((*(const uint32_t* const*)c)[1], 1, {U(c), 0});   // Create
    for (int i = 0; i < W.win->count; i++) W.widgets.push_back((Widget*)W.win->widgets[i]);
    if (getenv("VP_DEBUG_WORLD"))
        printf("  world: %d widgets; styles %d %d / %d %d / %d %d / %d %d\n", W.win->count, W.summary->style, W.summary->style2,
               W.blurb->style, W.blurb->style2, W.season_view->style, W.season_view->style2, W.standings->style, W.standings->style2);
    HS->frame_limit = 40;
}

// ---- each round: random states --------------------------------------------------------------------------------------------------
// every function-local Xlator of this group: {guard, bit, Xlator, key}
static const uint32_t k_xl[][4] = {
    {0x005cfe74, 0x01, 0x005cffd0, 0x004ff6c8}, {0x005cfe74, 0x02, 0x005cfe38, 0x004ff6e0}, {0x005d002c, 0x01, 0x005d0020, 0x004ff700},
    {0x005cfe64, 0x01, 0x005d0038, 0x004ff720}, {0x005cfe64, 0x02, 0x005cfe28, 0x004ff73c}, {0x005cfe64, 0x04, 0x005cf068, 0x004ff754},
    {0x005cfe64, 0x08, 0x005cf6f0, 0x004ff76c}, {0x005d0034, 0x01, 0x005cffa0, 0x004ff7ec}, {0x005d0034, 0x02, 0x005cf750, 0x004ff810},
    {0x005cf704, 0x01, 0x005cfe68, 0x004ff864}, {0x005cf704, 0x02, 0x005cfe18, 0x004ff874}, {0x005cf704, 0x04, 0x005d0000, 0x004ff88c},
    {0x005cf704, 0x08, 0x005cffe0, 0x004ff8a4}, {0x005cf704, 0x10, 0x005cf058, 0x004ff8bc}, {0x005cf704, 0x20, 0x005cffb0, 0x004ff8d8},
    {0x005cf704, 0x40, 0x005cfff0, 0x004ff8f0}, {0x005cf748, 0x01, 0x005cff90, 0x004ff910}, {0x005cf748, 0x02, 0x005cfe08, 0x004ff92c},
    {0x005cf6fc, 0x01, 0x005cffc0, 0x004ff948}, {0x005cf6fc, 0x02, 0x005cfe48, 0x004ff968}, {0x005d00ac, 0x01, 0x005d00b8, 0x004ffa84},
    {0x005d00ac, 0x02, 0x005d3588, 0x004ffa8c}, {0x005d00ac, 0x04, 0x005d00f0, 0x004ffa9c}, {0x005d00ac, 0x08, 0x005d0178, 0x004ffaa8},
    {0x005d00ac, 0x10, 0x005d0160, 0x004ffab8}, {0x005d00b0, 0x01, 0x005d00e0, 0x004ffae0}, {0x005d00b0, 0x02, 0x005d0128, 0x004ffaf4},
    {0x005d00b0, 0x04, 0x005d00c8, 0x004ffb0c}, {0x005d00b0, 0x08, 0x005d0058, 0x004ffb20}, {0x005d00b0, 0x10, 0x005d0048, 0x004ffb34},
    {0x005d00b0, 0x20, 0x005d0068, 0x004ffb3c}, {0x005d00b0, 0x40, 0x005d0188, 0x004ffb58}, {0x005d00b0, 0x80, 0x005d0080, 0x004ffb64},
    {0x005d00b4, 0x01, 0x005d0090, 0x004ffbc0}, {0x005d00b4, 0x02, 0x005d0140, 0x004ffbd4}, {0x005d00b4, 0x04, 0x005d01a0, 0x004ffbec},
    {0x005d00b4, 0x08, 0x005d00a0, 0x004ffc04}, {0x005d00b4, 0x10, 0x005d3550, 0x004ffc18}, {0x005d00b4, 0x20, 0x005d0150, 0x004ffc30},
    {0x005d360c, 0x01, 0x005d35d8, 0x004ffce8}, {0x005d360c, 0x02, 0x005d35a0, 0x004ffcf0}, {0x005d360c, 0x04, 0x005d35c0, 0x004ffd08},
    {0x005d360c, 0x08, 0x005d3610, 0x004ffd1c}, {0x005d5d4c, 0x01, 0x005d5d90, 0x004ffdf8}, {0x005d5d4c, 0x02, 0x005d5db0, 0x004ffdd8},
    {0x005d5d4c, 0x04, 0x005d5da0, 0x004ffdc4}, {0x005d5d4c, 0x08, 0x005d5d50, 0x004ffdb0}, {0x005d5d4c, 0x10, 0x005d5d60, 0x004ffd98},
    {0x005d5d4c, 0x20, 0x005d5d70, 0x004ffd84}, {0x005d5d4c, 0x40, 0x005d5d80, 0x004ffd70}, {0x005d3658, 0x01, 0x005d3668, 0x004ffe84},
    {0x005d3658, 0x02, 0x005d3648, 0x004ffea0}, {0x005d3658, 0x04, 0x005d3680, 0x004ffebc}, {0x005d3658, 0x08, 0x005d3628, 0x004ffed4},
    {0x005d3704, 0x01, 0x005d3730, 0x004fff28}, {0x005d3704, 0x02, 0x005d36e0, 0x004fff30}, {0x005d3704, 0x04, 0x005d3710, 0x004fff58},
    {0x005d36b0, 0x01, 0x005d36c8, 0x004fffb8}, {0x005d36b0, 0x02, 0x005d36f0, 0x004fffdc}, {0x005d5d0c, 0x01, 0x005d5d40, 0x0050007c},
    {0x005d5d0c, 0x02, 0x005d5d10, 0x00500064}, {0x005d5d0c, 0x04, 0x005d5d20, 0x0050004c}, {0x005d5d0c, 0x08, 0x005d5d30, 0x00500034},
    {0x005d41b8, 0x01, 0x005d4198, 0x00500874}, {0x005d41b8, 0x02, 0x005d41f0, 0x0050087c},
};
static const char k_chars[] = "abcdefghij klmnopqrstuvwxyz ABCDEFGH 0123456789 .,!-";
static void random_text(char* s, int maxlen) {
    const int n = irange(0, maxlen);
    for (int i = 0; i < n; i++) s[i] = k_chars[rnd() % (sizeof k_chars - 1)];
    s[n] = 0;
}
static void randomize_world() {
    random_info((uint8_t*)(uintptr_t)S_INFO, chance(97));
    if (chance(70)) memcpy((void*)(uintptr_t)S_INFO_SAVED, (const void*)(uintptr_t)S_INFO, S_INFO_SIZE);
    else random_info((uint8_t*)(uintptr_t)S_INFO_SAVED, true);
    for (int s = 0; s < 8; s++) random_info((uint8_t*)(uintptr_t)(S_CH_INFOS + S_INFO_SIZE * (uint32_t)s), chance(95));
    random_season(g_season);
    // the season's end reached now and then (do_event's last event, the standings' screens)
    if (chance(30)) CA_G32(S_EVENT) = *(int32_t*)g_season - (chance(70) ? 1 : 0);
    for (int i = 0; i < 16; i++) *(uint32_t*)(g_trackinfo + i * 0x20 + 0x10) = fbits(fval(30.0f, 200.0f));
    for (int d = 0; d < 8; d++)
        for (int k = 0; k < 16; k++)
            *(uint32_t*)(g_drivers + d * 0x220 + 0x98 + k * 0x18) = chance(15) ? rnd() : fbits(fval(-5.0f, 20.0f));
    memset(W.world, 0, 0xcd4);
    *(int32_t*)W.world = 3; *(int32_t*)(W.world + 4) = 0xcd4;
    strcpy((char*)W.world + 8, k_tracks[rnd() % 9]);
    *(int32_t*)(W.world + 0xca8) = irange(0, 8);
    for (int i = 0; i < 16; i++) {
        uint8_t* e = W.world + 0x28 + i * 0xc8;
        *(int32_t*)e = chance(70) ? 1 : irange(0, 3);
        random_text((char*)e + 4, 11);
        strcpy((char*)e + 0x11, "viper");
        *(int32_t*)(e + 0xc0) = irange(-9, 9);
    }
    for (int i = 0; i < 0x31; i++) ((uint32_t*)W.results)[i] = chance(50) ? (uint32_t)irange(-1, 10) : rnd();
    for (int i = 0; i < 10; i++) ((uint32_t*)W.opts)[i] = rnd();
    for (uint32_t i = 0; i < 0x800; i++) W.block[i] = (uint8_t)rnd();
    for (int i = 0; i < 16; i++) W.ints[i] = chance(90) ? irange(0, 7) : irange(-2, 9);
    for (int i = 0; i < 8; i++) random_text(W.texts[i], i < 4 ? 12 : 40);
    memset(W.track_buf, 0x5a, 0x40);
    for (int i = 0; i < 16; i++) W.flags[i] = (uint8_t)rnd();
    memset(W.scratch, 0xa5, 0x200);
    // the controls
    W.status->x = irange(0, 100); W.status->y = irange(0, 100); W.status->w = irange(0, 500); W.status->h = irange(0, 400);
    for (CareerSummary* s : {W.summary, W.blurb}) {
        s->x = irange(-20, 600); s->y = irange(-20, 460); s->w = irange(-10, 300); s->h = irange(-10, 200);
        s->info = (const uint8_t*)(uintptr_t)(chance(50) ? S_INFO : S_CH_INFOS + S_INFO_SIZE * (uint32_t)irange(0, 7));
    }
    for (TwoStyles* s : {W.season_view, W.standings}) {
        s->x = irange(-20, 600); s->y = irange(-20, 460); s->w = irange(-10, 600); s->h = irange(-10, 400);
    }
    W.track_image->x = irange(-20, 600); W.track_image->y = irange(-20, 460);
    for (Widget* g : W.widgets) { g->visible = chance(85); g->enabled = chance(85); g->dirty = chance(50); }
    // the stubs' world
    HS->time = (int32_t)rnd();
    HS->frames = 0;
    HS->frame_limit = 40;
    HS->grab_fail_at = chance(20) ? irange(1, 6) : -1;
    HS->mouse_x = irange(0, 639); HS->mouse_y = irange(0, 479);
    HS->script_n = HS->script_pos = 0;
    HS->rs = rnd() | 1;
    for (int i = 0; i < 8; i++) HS->scan_mask[i] = chance(30) ? rnd() : rnd() & rnd() & rnd();
    HS->prerace_calls = 0; HS->prerace_end = irange(1, 6);
    HS->opt_mask = rnd();
    for (int i = 0; i < 8; i++) HS->opt_val[i] = chance(85) ? irange(0, 3) : (int32_t)rnd();
    HS->userdir_len = irange(2, 60);
    HS->file_open_fail = irange(0, 30);
    HS->file_create_fail = irange(0, 30);
    HS->file_pos = 0; HS->file_img = 0;
    random_file(0);
    random_file(1);
    HS->res_fail = chance(90) ? 0 : 20;
    HS->track_count = chance(90) ? 9 : irange(0, 9);
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
    UI_GU32(S_LOUNGE) = U(g_lounge_obj);
    UI_GP(uint8_t, S_SEASON) = chance(98) ? g_season : 0;
    UI_GU32(S_UPGRADE_SET) = chance(50) ? 0 : 0x00ab0000;
    UI_GU32(S_CAREER_UPGRADES) = chance(50) ? 0 : S_UPGRADES_IN_INFO;
    UI_G32(S_LOG) = chance(50) ? 0 : 0x0f11e2;
    UI_G32(S_SLOT) = irange(0, 7);
    UI_G8(S_NEW_CLASS) = (uint8_t)(chance(80) ? 0 : 1);
    UI_G32(S_CH_SLOT) = chance(95) ? irange(0, 7) : irange(-1, 8);
    for (int i = 0; i < 8; i++) UI_G8(S_CH_USED + i) = (uint8_t)(rnd() & 1);
    UI_G32(S_PS_AMOUNT) = chance(50) ? -1 : irange(0, 7);
    UI_G8(S_PS_RACE_MONEY) = (uint8_t)(rnd() & 1);
    UI_G8(S_PS_CLICKED) = (uint8_t)(chance(70) ? 0 : 1);
    UI_G32(S_PS_SPLASH_TIME) = chance(50) ? HS->time - irange(0, 20000) : (int32_t)rnd();
    UI_G32(S_TS_TRACK) = chance(90) ? irange(0, 7) : irange(-1, 12);
    UI_G8(S_TS_REVERSED) = (uint8_t)(rnd() & 1);
    UI_G32(S_CH_GROUP_EMPTY) = (int32_t)(1u << irange(0, 8));
    UI_G32(S_CH_GROUP_USED) = (int32_t)(1u << irange(0, 8));
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
    static const uint16_t keys[] = {0x1b, 0xd, 0x20, 8, 0x31, 0x32, 0x33, 'a', 'r', 'u', 'e', 't', 's', 0x121, 0x122, 0x124,
                                    0x170, 0x171, 0x175, 9, 'P', 'g', 'q', 0x123};
    const int n = irange(0, maxops);
    for (int i = 0; i < n && i < 62; i++) {
        int32_t* op = HS->script[i];
        const uint32_t r = rnd() % 10;
        op[1] = op[2] = 0;
        if (r < 4) { op[0] = OP_MOVE_W; op[1] = irange(0, 40); op[2] = irange(0, 479); }
        else if (r < 5) { op[0] = OP_MOVE_XY; op[1] = irange(-10, 650); op[2] = irange(-10, 490); }
        else if (r < 6) op[0] = chance(50) ? OP_DOWN : OP_RDOWN;
        else if (r < 7) op[0] = chance(50) ? OP_UP : OP_RUP;
        else if (r < 9) { op[0] = OP_KEY; op[1] = keys[rnd() % (sizeof keys / sizeof keys[0])]; }
        else op[0] = OP_NOP;
        if (op[0] == OP_MOVE_W && chance(75)) {
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
        {0x004e59b0, 0x004e59b4},                                       // "%d" (the standings')
        {0x004eb400, 0x004eb444},
        {0x004f665c, 0x004f6698}, {0x004f6b78, 0x004f7228}, {0x004f7230, 0x004f7300},   // the ui library's keys and strings
        {0x004efbf8, 0x004efbfc},
        {0x004ff5d8, 0x005008b0},                                       // the career's strings (and its initialised pointers)
        {0x005024ac, 0x005024c8},                                       // the CRT's FPU flags
        {0x00502630, 0x00502680}, {0x005026f8, 0x00502730}, {0x00502780, 0x005028c8},   // its jump tables and constants
        {0x005228d4, 0x005228d8}, {0x005228f4, 0x005228f8},
        {0x00578cd0, 0x00578cdc}, {0x00578d50, 0x00578d5c}, {0x00578d60, 0x00578d6c}, {0x00578e88, 0x00578e94},
        {0x00578e98, 0x00578ea4}, {0x00578eb0, 0x00578ebc}, {0x00578ec0, 0x00578ecc}, {0x00578ef8, 0x00578f04},
        {0x00579050, 0x0057905c},
        {0x00578d08, 0x00578d40},                                       // the end item
        {0x00578d48, 0x00578d4c}, {0x00578e78, 0x00578e7c}, {0x00578f40, 0x00578f44}, {0x00578e80, 0x00578e84},
        {0x005790e0, 0x005796e0},                                       // the styles (the controls' dynamic ones)
        {0x00579730, 0x00579778},                                       // the running window, the window table
        {0x005cf078, 0x005cf6ec}, {0x005cf790, 0x005cfe04},             // the CareerInfo and its saved copy (randomised)
        {0x005d01b0, 0x005d3550},                                       // the chooser's eight
        {0x005d0074, 0x005d0078},                                       // the chooser's list
    };
    for (auto& k : keep) memcpy(DATA + (k[0] - 0x004e1000), keepfrom.data + (k[0] - 0x004e1000), k[1] - k[0]);
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static uint32_t b8(uint32_t v) { return chance(50) ? (rnd() & ~0xffu) | (v & 0xff) : v; }
static uint32_t canvas_arg() { return chance(70) ? U(W.screen) : U(W.canvas2); }
static const char* const k_formats[] = {"Career: %s  class: %d", "Qualify: %s %1.1f", "Race: %s place: %d", "Season: rank %d",
                                         "Date: %s", "----------------------------------------------"};
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const char* nm = f.name;
    uint32_t* a = f.fast ? w + 2 : w;
#define IS(s) (!strcmp(nm, s))
#define HAS(s) (strstr(nm, s) != 0)
    if (HAS("deleting destructor")) {
        a[0] = rnd() & 3;
        if (HAS("SeasonViewer")) w[0] = U(W.season_view);
        else if (HAS("StandingsViewer")) w[0] = U(W.standings);
        else { w[0] = U(W.scratch); ((uint32_t*)W.scratch)[0] = VT_UICustomControl; }
        if (HAS("CareerBlurb") || HAS("CareerSummary")) w[0] = U(chance(50) ? W.summary : W.blurb);
        return true;
    }
    if (IS("CareerGiveAward")) a[0] = (uint32_t)irange(0, 6);
    else if (IS("CareerLog")) {
        const int k = (int)(rnd() % 6);
        a[0] = U(k_formats[k]);
        a[1] = chance(80) ? U(W.texts[rnd() % 8]) : (uint32_t)irange(0, 9);
        if (k == 1) { const double d = (double)fval(10, 200); memcpy(&a[2], &d, 8); }
        else a[2] = (uint32_t)irange(-3, 12);
        if (k == 0 || k == 2) { a[1] = U(W.texts[rnd() % 8]); }
        if (k == 3) a[1] = (uint32_t)irange(0, 8);
    }
    else if (IS("career_main")) a[0] = (uint32_t)irange(0, 7);
    else if (IS("do_event")) a[0] = (uint32_t)(chance(90) ? CA_G32(S_EVENT) : irange(0, 11));
    else if (IS("fake_results")) { a[0] = (uint32_t)irange(-1, 9); a[1] = U(W.results); }
    else if (IS("sort_carlist(career.obj)")) a[0] = U(W.world);
    else if (HAS("CareerStatus::")) {
        w[0] = U(W.status);
        if (HAS("Draw")) a[0] = canvas_arg();
    }
    else if (IS("CareerGetDriver") || IS("CareerGetDriverName")) a[0] = (uint32_t)(chance(90) ? irange(0, 7) : irange(-1, 8));
    else if (IS("CareerGetClassName")) a[0] = (uint32_t)irange(-1, 5);
    else if (IS("null_postrace")) a[0] = U(W.world);
    else if (IS("get_options")) a[0] = U(W.opts);
    else if (IS("CareerLoadSlot") || IS("CareerSaveSlot") || IS("career_load") || IS("career_save")) {
        a[0] = (uint32_t)irange(0, 7);
        a[1] = chance(60) ? U(W.info_buf) : (uint32_t)S_INFO;
    }
    else if (IS("CareerDestroySlot") || IS("get_career_filename")) a[0] = (uint32_t)irange(-1, 8);
    else if (IS("career_menu")) a[0] = U(W.flags + (rnd() & 7));
    else if (IS("career_menu_idle") || IS("show_race_money") || IS("season_splash_idle") || IS("ClickThrough::Idle")) a[0] = U(W.ints);
    else if (IS("driver_compare")) { a[0] = U(W.ints + (rnd() & 7)); a[1] = U(W.ints + (rnd() & 7)); }
    else if (IS("set_class")) a[0] = (uint32_t)irange(0, 3);
    else if (IS("checksum")) { a[0] = U(W.block + (rnd() & 0xff)); a[1] = (uint32_t)(chance(80) ? irange(0, 0x674) : irange(-5, 0x700)); }
    else if (IS("xor_block")) { a[0] = U(W.block + (rnd() & 0xff)); a[1] = (uint32_t)(chance(80) ? irange(0, 0x674) : irange(-5, 0x700)); a[2] = b8(0xa4); }
    else if (IS("credit_account")) {
        a[0] = U(g_season + 0x24 + 0x58 * (rnd() % 12));
        a[1] = U(W.results);
        a[2] = chance(70) ? (uint32_t)S_DRIVER_MAP : U(W.ints);
        for (int i = 0; i < 8; i++) *(int32_t*)(W.results + 4 + 12 * i) = irange(-1, 10);
    }
    else if (IS("CareerSeasonGet")) a[0] = U(chance(80) ? "season1.ssn" : W.texts[0]);
    else if (IS("could_xlate_money")) a[0] = chance(50) ? (uint32_t)irange(0, 50000) : rnd();
    else if (IS("CareerSeasonForget")) a[0] = U(g_season);
    else if (IS("CareerSummary::CareerSummary")) { w[0] = U(W.scratch); a[0] = chance(50) ? S_INFO : S_CH_INFOS; }
    else if (IS("CareerSummary::~CareerSummary")) w[0] = U(chance(50) ? W.summary : W.blurb);
    else if (IS("CareerSummary::draw_entry")) {
        w[0] = U(chance(50) ? W.summary : W.blurb);
        a[0] = U(W.texts[rnd() % 8]); a[1] = U(W.texts[rnd() % 8]); a[2] = (uint32_t)irange(-10, 500);
    }
    else if (IS("CareerSummary::Draw")) { w[0] = U(chance(50) ? W.summary : W.blurb); a[0] = canvas_arg(); }
    else if (HAS("CareerBlurb::")) { w[0] = U(W.blurb); a[0] = rnd() & 3; a[1] = U(W.flags); }
    else if (IS("SeasonViewer::SeasonViewer") || IS("StandingsViewer::StandingsViewer")) w[0] = U(W.scratch);
    else if (HAS("SeasonViewer::") && !HAS(" helper")) { w[0] = U(W.season_view); if (HAS("Draw")) a[0] = canvas_arg(); else { a[0] = rnd() & 3; a[1] = U(W.flags); } }
    else if (HAS("StandingsViewer::") && !HAS(" helper")) { w[0] = U(W.standings); if (HAS("Draw")) a[0] = canvas_arg(); }
    else if (HAS("TrackImage::")) { w[0] = U(W.track_image); if (HAS("Draw")) a[0] = canvas_arg(); else { a[0] = rnd() & 3; a[1] = U(W.flags); } }
    else if (HAS("ClickThrough::")) { w[0] = U(W.click); if (HAS("Draw")) a[0] = canvas_arg(); else { a[0] = (uint32_t)irange(0, 639); a[1] = (uint32_t)irange(0, 479); } }
    else if (IS("do_race")) { a[0] = U(chance(90) ? k_tracks[rnd() % 9] : W.texts[rnd() % 4]); a[1] = b8(rnd() & 1); }
    else if (IS("testing_menu")) { a[0] = U(W.track_buf); a[1] = U(W.flags + 8); }
    else if (IS("PostSeasonDo")) a[0] = (uint32_t)(chance(40) ? -1 : irange(0, 8));
    else if (IS("show_season_money") || IS("show_winning_season_money") || HAS("_cb")) a[0] = (uint32_t)irange(-3, 3);
#undef IS
#undef HAS
    return true;
}
// the functions that run a dialog or a loop (fewer rounds, longer scripts)
static bool is_modal(const char* nm) {
    static const char* const m[] = {"CareerDo", "career_main", "do_event", "career_menu", "CareerChooser", "create_cb", "EventsDo",
                                    "TestingDo", "do_race", "testing_menu", "PostSeasonDo", "show_season_win_splash", "NewClassDo",
                                    "RankingDo", "back_cb", "save_cb(career.obj)", "CareerDestroySlot", "delete_cb(chooser.obj)",
                                    "upgrade_cb", "ranking_cb", "career_menu_idle", "refresh"};
    for (const char* s : m) if (!strcmp(nm, s)) return true;
    return false;
}
// the end-of-season screens get more rounds (no in-game check this step: a season has to be finished)
static bool is_season_end(const char* nm) {
    static const char* const m[] = {"PostSeasonDo", "show_race_money", "show_season_money", "show_season_win_splash",
                                    "show_winning_season_money", "season_splash_idle", "NewClassDo", "StandingsViewer::StandingsViewer",
                                    "StandingsViewer::Draw", "StandingsViewer::scalar deleting destructor", "ClickThrough::MouseDown",
                                    "ClickThrough::MouseRDown", "ClickThrough::Idle", "do_event"};
    for (const char* s : m) if (!strcmp(nm, s)) return true;
    return false;
}

// ---- the fix build: rounds kept out of the comparison ------------------------------------------------------------------------
// (before the original runs) create_cb with a default name's translation over 15 characters: the fixed copy stops at the
// 16-byte name, where the original's ran on (left unterminated when no saved name replaces it). Every way the Xlator can be
// found (built fresh or stale, or not built) gives xl_text of its key.
static bool fx_pre_case(const Ent& f, const uint32_t*) {
    if (!strcmp(f.name, "create_cb")) return strlen(xl_text(0x004ffb3c)) > 15;      // Career:DefaultPlayerName
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
// the recorders: FileCreate failing for an empty name (as CreateFileA does); the dialogs answering g_fx_dialog; the
// options' player_name read recording the buffer it's given (and writing nothing); GetTrackName answering g_fx_track
static int32_t __cdecl fx_FileCreate(const char* name) { L('FCRT'); LS(name); return readable(name, 1) && name[0] ? 0x0f11e1 : 0; }
static int32_t g_fx_dialog;
static uint32_t g_fx_dlg_off;                               // (the dialog's caller's frame: 0x200 bytes from the UIDialog + this)
static uint8_t g_fx_dlg[0x200];
static int32_t __cdecl fx_UIDoDialog(const void* d, int32_t, int32_t, int32_t, int32_t, uint8_t) {
    L('DLG '); L(P(d));
    if (g_fx_dlg_off && readable((const uint8_t*)d + g_fx_dlg_off, 0x200)) memcpy(g_fx_dlg, (const uint8_t*)d + g_fx_dlg_off, 0x200);
    return g_fx_dialog;
}
// UIStyleDraw: each text drawn recorded
static std::vector<std::string> g_fx_draws;
static void __cdecl fx_UIStyleDraw(int32_t style, int32_t x, int32_t y, const char* s, uint32_t fl) {
    L('SDRW'); L((uint32_t)style); L((uint32_t)x); L((uint32_t)y); LS(s); L(fl);
    g_fx_draws.push_back(readable(s, 1) ? std::string(s, strnlen(s, 0x2000)) : std::string("?"));
}
static uint8_t g_fx_opts[0x40];
static int g_fx_opts_n;
static void __cdecl fx_OptionsGetS(const char* sec, const char* key, char* out, int32_t n) {
    L('OPTS'); LS(sec); LS(key); L((uint32_t)n);
    if (readable(out, 0x40)) memcpy(g_fx_opts, out, 0x40);
    g_fx_opts_n++;
}
static const char* g_fx_track;
static const char* __cdecl fx_GetTrackName(int32_t i) { L('GTRN'); L((uint32_t)i); return g_fx_track; }
// the tests' world: pristine, every function-local Xlator built (fresh), the statics the code follows, the stubs' defaults
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
    UI_GU32(S_LOUNGE) = U(g_lounge_obj);
    UI_GP(uint8_t, S_SEASON) = g_season;
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
    HS->prerace_calls = 0; HS->prerace_end = 2;
    HS->opt_mask = 0;
    for (int i = 0; i < 8; i++) HS->opt_val[i] = 1;
    HS->userdir_len = 20;
    HS->file_open_fail = 0; HS->file_create_fail = 0; HS->file_pos = 0; HS->file_img = 0;
    HS->res_fail = 0;
    HS->track_count = 9;
    g_fx_dialog = -1;
    g_fx_dlg_off = 0;
    g_fx_track = "uptown";
}
static char g_fx_s[8][0x1000];
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
    static Footprint fp;
    auto footprint = [&](const Ent& f, std::initializer_list<uint32_t> w) {
        uint32_t words[72] = {};
        int i = 0;
        for (uint32_t x : w) words[i++] = x;
        fp.n = 0; fp.replay_only = 0; fp.pure = false;
        f.fp(fp, words);
    };
    char* const fname = (char*)(uintptr_t)S_FILENAME;

    // ---- 1. get_career_filename: a long user directory ----
    {
        const Ent& f = fx_fn("get_career_filename");
        struct { int32_t dir, slot; } cases[] = {{246, 7}, {254, 0}, {245, -1}};   // (the stub's longest directory is 254)
        for (auto& c : cases) {
            fx_reset();
            HS->userdir_len = c.dir;
            memset(fname, 0x5a, 0x108);
            mem_save(g_snap);
            r = fx_run(f, true, {(uint32_t)c.slot});
            sprintf(m, "get_career_filename, a %d-character user directory, slot %d: a clean return, its buffer", c.dir, c.slot);
            fx_check(fx_clean(f, r) && r.ret == S_FILENAME, m, &r);
            sprintf(m, "get_career_filename, a %d-character user directory, slot %d: the name \"\"", c.dir, c.slot);
            fx_check(fname[0] == 0, m);
            o = fx_outside(g_snap, {{fname, 1}});
            sprintf(m, "get_career_filename, a %d-character user directory, slot %d: nothing written but the name's terminator", c.dir, c.slot);
            fx_check(o == 0, m, 0, o);
        }
        // the boundaries: names of 263 characters
        fx_reset();
        HS->userdir_len = 245;
        fx_same(f, {7}, "get_career_filename, a 245-character user directory, slot 7 (a name of 263 characters)");
        fx_reset();
        HS->userdir_len = 244;
        fx_same(f, {(uint32_t)-1}, "get_career_filename, a 244-character user directory, slot -1 (a name of 263 characters)");
    }

    // ---- 2. career_save: a long user directory (get_career_filename's rewrite called, FileCreate failing for "") ----
    {
        const Ent& f = fx_fn("career_save");
        FxPatch gcf(F_get_career_filename, (void*)&career_main::get_career_filename_c);
        FxPatch fc(F_FileCreate, (void*)&fx_FileCreate);
        fx_reset();
        HS->userdir_len = 254;
        mem_save(g_snap);
        r = fx_run(f, true, {3, S_INFO});
        fx_check(fx_clean(f, r), "career_save, a 254-character user directory: a clean return", &r);
        fx_check(!fx_logged('FMKD'), "career_save, a 254-character user directory: no directory made");
        fx_check(fx_logged('FCRT') && fx_logged('LOGR') && !fx_logged('FWRT'),
                 "career_save, a 254-character user directory: the file not made (\"\"), reported, nothing written");
        o = fx_outside(g_snap, {{fname, 0x108}});
        fx_check(o == 0, "career_save, a 254-character user directory: nothing written but the file name's buffer", 0, o);
        // the boundaries: the longest directory that's made (its file's name then too long: ""), and the longest that saves
        fx_reset();
        HS->userdir_len = 253;
        fx_same(f, {3, S_INFO}, "career_save, a 253-character user directory (\"<dir>career\" of 259; the file's name \"\")");
        fx_check(fx_logged('FMKD') && !fx_logged('FWRT'), "career_save, a 253-character user directory: the directory made, no file");
        fx_reset();
        HS->userdir_len = 245;
        fx_same(f, {3, S_INFO}, "career_save, a 245-character user directory (the file's name of 263)");
        fx_check(fx_logged('FWRT'), "career_save, a 245-character user directory: the career written");
    }

    // ---- 3. set_class: a long class name ----
    {
        const Ent& f = fx_fn("set_class");
        static const uint32_t k_class_xl[4] = {0x005d0038, 0x005cfe28, 0x005cf068, 0x005cf6f0};
        for (int n : {48, 2000}) {
            fx_reset();
            const char* cn = mk(0, 'c', n);
            for (uint32_t a : k_class_xl) UI_GU32(a + 4) = U(cn);
            mem_save(g_snap);
            r = fx_run(f, true, {2});
            sprintf(m, "set_class, a %d-character class name: a clean return", n);
            fx_check(fx_clean(f, r), m, &r);
            sprintf(m, "set_class, a %d-character class name: its first 47 characters", n);
            fx_check(first(cn, 47) == std::string((const char*)(uintptr_t)S_CLASS_NAME), m);
            o = fx_outside(g_snap, {{(void*)(uintptr_t)S_CLASS, 4}, {(void*)(uintptr_t)S_SEASON, 4}, {(void*)(uintptr_t)S_CLASS_NAME, 0x30}});
            sprintf(m, "set_class, a %d-character class name: nothing written but the class, the season and the name (the saved "
                       "CareerInfo untouched)", n);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        for (uint32_t a : k_class_xl) UI_GU32(a + 4) = U(mk(0, 'c', 47));
        fx_same(f, {2}, "set_class, a 47-character class name");
    }

    // ---- 4. CareerStatus::Callback: a long week line ----
    {
        const Ent& f = fx_fn("CareerStatus::Callback");
        CareerStatus* st = W.status;
        struct { int n; int32_t week; } cases[] = {{62, 5}, {55, 999999999}, {300, -7}};
        for (auto& c : cases) {
            fx_reset();
            const char* wk = mk(0, 'w', c.n);
            UI_GU32(0x005d0024) = U(wk);
            CA_G32(S_WEEK) = c.week;
            CA_G32(S_FUNDS) = 12345;
            CA_G32(S_SEASONS) = 2;
            memset((void*)st->week, 0x5a, 0xc0);
            footprint(f, {U(st), 0, 0, 0});
            mem_save(g_snap);
            r = fx_run(f, true, {U(st), 0, 0, 0});
            sprintf(m, "CareerStatus::Callback, a %d-character translation, week %d: a clean return", c.n, c.week + 1);
            fx_check(fx_clean(f, r), m, &r);
            char e[0x200];
            sprintf(e, "%s %d", wk, c.week + 1);
            sprintf(m, "CareerStatus::Callback, a %d-character translation, week %d: the line's first 63 characters, the funds and "
                       "season lines as formatted", c.n, c.week + 1);
            fx_check(first(e, 63) == std::string(st->week) && !strcmp(st->funds, "$12345") && !strcmp(st->season, "3"), m);
            o = fx_outside_fp(g_snap, fp);
            sprintf(m, "CareerStatus::Callback, a %d-character translation: nothing written outside its texts", c.n);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        UI_GU32(0x005d0024) = U(mk(0, 'w', 61));
        CA_G32(S_WEEK) = 5;
        fx_same(f, {U(st), 0, 0, 0}, "CareerStatus::Callback, a 61-character translation and week 6 (a line of 63)");
    }

    // ---- 5. create_cb: a long default name (no player_name saved; the dialog answered Create) ----
    {
        const Ent& f = fx_fn("create_cb");
        FxPatch dlg(uit::F_UIDoDialog, (void*)&fx_UIDoDialog);
        FxPatch opt(F_OptionsGetS, (void*)&fx_OptionsGetS);
        for (int n : {16, 40, 700}) {
            fx_reset();
            const char* dn = mk(0, 'd', n);
            UI_GU32(0x005d006c) = U(dn);
            CA_G32(S_CH_SLOT) = 3;
            uint8_t* slot = (uint8_t*)(uintptr_t)(S_CH_INFOS + 3 * S_INFO_SIZE);
            g_fx_dialog = -2;
            g_fx_opts_n = 0;
            mem_save(g_snap);
            r = fx_run(f, true, {0});
            sprintf(m, "create_cb, a %d-character default name: a clean return, Create", n);
            fx_check(fx_clean(f, r) && r.ret == 1, m, &r);
            bool after = true;                                      // what the frame held after the name: the stack's fill
            for (int k = 16; k < 0x40; k++) after &= g_fx_opts[k] == 0xcd;
            sprintf(m, "create_cb, a %d-character default name: the name field holds its first 15 characters, nothing after it written", n);
            fx_check(g_fx_opts_n == 1 && first((const char*)g_fx_opts, 16) == first(dn, 15) && g_fx_opts[15] == 0 && after, m);
            sprintf(m, "create_cb, a %d-character default name: the new career named with its first 15 characters", n);
            fx_check(first((const char*)slot, 16) == first(dn, 15), m);
            o = fx_outside(g_snap, {{slot, S_INFO_SIZE}, {fname, 0x108}});
            sprintf(m, "create_cb, a %d-character default name: nothing written but the slot's career and the file's name", n);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        UI_GU32(0x005d006c) = U(mk(0, 'd', 15));
        CA_G32(S_CH_SLOT) = 3;
        g_fx_dialog = -2;
        fx_same(f, {0}, "create_cb, a 15-character default name, Create");
    }

    // ---- 6. testing_menu: a long track name (the dialog answered Test) ----
    {
        const Ent& f = fx_fn("testing_menu");
        FxPatch dlg(uit::F_UIDoDialog, (void*)&fx_UIDoDialog);
        FxPatch gtn(F_GetTrackName, (void*)&fx_GetTrackName);
        char* tb = W.track_buf;
        uint8_t* rev = W.flags + 8;
        for (int n : {32, 300}) {
            fx_reset();
            g_fx_track = mk(0, 't', n);
            g_fx_dialog = -2;
            CA_G32(S_TS_TRACK) = 2;
            CA_G8(S_TS_REVERSED) = 1;
            memset(tb, 0x5a, 0x40);
            *rev = 0;
            mem_save(g_snap);
            r = fx_run(f, true, {U(tb), U(rev)});
            sprintf(m, "testing_menu, a %d-character track name, Test: a clean return, as Back (0)", n);
            fx_check(fx_clean(f, r) && r.ret == 0, m, &r);
            bool untouched = true;
            for (int k = 0; k < 0x40; k++) untouched &= (uint8_t)tb[k] == 0x5a;
            sprintf(m, "testing_menu, a %d-character track name: the caller's buffer untouched", n);
            fx_check(untouched, m);
            o = fx_outside(g_snap, {{rev, 1}, {g_arena + A_HEAP, ARENA_BYTES - A_HEAP}});
            sprintf(m, "testing_menu, a %d-character track name: nothing written but the reversed flag and the heap (its list)", n);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        g_fx_track = mk(0, 't', 31);
        g_fx_dialog = -2;
        memset(tb, 0x5a, 0x40);
        fx_same(f, {U(tb), U(rev)}, "testing_menu, a 31-character track name, Test");
        fx_check(first(tb, 0x40) == std::string(31, 't'), "testing_menu, a 31-character track name: copied");
    }

    // ---- 7. do_race: a long track name ----
    {
        const Ent& f = fx_fn("do_race");
        for (int n : {32, 300}) {
            fx_reset();
            const char* t = mk(0, 't', n);
            mem_save(g_snap);
            r = fx_run(f, true, {U(t), 1});
            sprintf(m, "do_race, a %d-character track name: a clean return, no race (nothing called)", n);
            fx_check(fx_clean(f, r) && g_log.n == 0, m, &r);
            o = fx_outside(g_snap, {});
            sprintf(m, "do_race, a %d-character track name: nothing written", n);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        fx_same(f, {U(mk(0, 't', 31)), 1}, "do_race, a 31-character track name");
        fx_check(fx_logged('LDRC') && fx_logged('PRDO'), "do_race, a 31-character track name: the race loaded, the pre-race screen run");
    }


    // ---- 10. TrackImage::Draw: a long track name (its picture) ----
    {
        const Ent& f = fx_fn("TrackImage::Draw");
        FxPatch gtn(F_GetTrackName, (void*)&fx_GetTrackName);
        const std::initializer_list<uint32_t> args = {U(W.track_image), 0, U(W.screen)};
        for (int n : {76, 300}) {
            fx_reset();
            g_fx_track = mk(0, 't', n);
            mem_save(g_snap);
            r = fx_run(f, true, args);
            sprintf(m, "TrackImage::Draw, a %d-character track name: a clean return, no picture loaded or drawn", n);
            fx_check(fx_clean(f, r) && !fx_logged('GSTP') && !fx_logged('DSTP'), m, &r);
            o = fx_outside(g_snap, {{(void*)(uintptr_t)S_GX_CANVAS, 4}});
            sprintf(m, "TrackImage::Draw, a %d-character track name: nothing written but the current canvas", n);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        g_fx_track = mk(0, 't', 75);
        fx_same(f, args, "TrackImage::Draw, a 75-character track name (\"<name>.stp\" of 79)");
        fx_check(fx_logged('GSTP') && fx_logged('DSTP'), "TrackImage::Draw, a 75-character track name: its picture loaded and drawn");
    }

    // ---- 11. EventsDo: long Season / Week translations (the dialog answered at once; its frame's two texts recorded) ----
    {
        const Ent& f = fx_fn("EventsDo");
        FxPatch dlg(uit::F_UIDoDialog, (void*)&fx_UIDoDialog);
        uint8_t* inf = (uint8_t*)(uintptr_t)S_INFO;
        for (int n : {254, 600}) {
            fx_reset();
            const char* se = mk(0, 's', n);
            const char* wk = mk(1, 'w', n + 7);
            UI_GU32(0x005d35a4) = U(se);
            UI_GU32(0x005d35c4) = U(wk);
            *(int32_t*)(inf + 0x11c) = 0;
            *(int32_t*)(inf + 0x120) = 41;
            g_fx_dialog = -1;
            g_fx_dlg_off = 0x224;                                // the UIDialog at +0xc: the texts at +0x230, +0x330
            mem_save(g_snap);
            r = fx_run(f, true, {});
            sprintf(m, "EventsDo, %d- and %d-character translations: a clean return", n, n + 7);
            fx_check(fx_clean(f, r) && r.ret == 0, m, &r);
            char e0[0x400], e1[0x400];
            sprintf(e0, "%s %d", wk, 42);
            sprintf(e1, "%s %d", se, 1);
            sprintf(m, "EventsDo, %d- and %d-character translations: each text its first 255 characters", n, n + 7);
            fx_check(first((const char*)g_fx_dlg, 0x100) == first(e0, 255) && first((const char*)g_fx_dlg + 0x100, 0x100) == first(e1, 255), m);
            o = fx_outside(g_snap, {{(void*)(uintptr_t)0x005790c0, 0x620}, {g_arena + A_HEAP, ARENA_BYTES - A_HEAP}});   // (the ui
            // library's style table and its state before it: the SeasonViewer's two dynamic styles added and removed)
            sprintf(m, "EventsDo, %d- and %d-character translations: nothing written but the styles and the heap", n, n + 7);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        UI_GU32(0x005d35a4) = U(mk(0, 's', 253));
        UI_GU32(0x005d35c4) = U(mk(1, 'w', 252));
        *(int32_t*)(inf + 0x11c) = 0;
        *(int32_t*)(inf + 0x120) = 41;
        g_fx_dialog = -2;
        g_fx_dlg_off = 0x224;
        fx_same(f, {}, "EventsDo, texts of 255 characters");
    }

    // ---- 12. SeasonViewer::Draw: long Laps / Pts translations (the drawn texts recorded) ----
    {
        const Ent& f = fx_fn("SeasonViewer::Draw");
        FxPatch sd(uit::F_UIStyleDraw, (void*)&fx_UIStyleDraw);
        const std::initializer_list<uint32_t> args = {U(W.season_view), 0, U(W.screen)};
        uint8_t* inf = (uint8_t*)(uintptr_t)S_INFO;
        auto setup = [&](const char* laps, const char* pts) {
            fx_reset();
            *(int32_t*)g_season = 3;
            for (int e = 0; e < 3; e++) {
                uint8_t* ev = g_season + 0x24 + 0x58 * e;
                strcpy((char*)ev, "uptown");
                *(int32_t*)(ev + 0x14) = 5;
                for (int k = 0; k < 8; k++) { *(int32_t*)(ev + 0x18 + 4 * k) = 1000; *(int32_t*)(ev + 0x38 + 4 * k) = 10 - k; }
            }
            *(int32_t*)(inf + 0x128) = 2;
            for (int e = 0; e < 3; e++) *(int32_t*)(inf + 0x530 + 4 * e) = 1;
            UI_GU32(0x005d5d94) = U(laps);
            UI_GU32(0x005d5db4) = U(pts);
            g_fx_draws.clear();
        };
        for (int n : {254, 600}) {
            const char* la = mk(0, 'l', n);
            const char* pt = mk(1, 'p', n + 3);
            setup(la, pt);
            mem_save(g_snap);
            r = fx_run(f, true, args);
            sprintf(m, "SeasonViewer::Draw, %d- and %d-character translations: a clean return", n, n + 3);
            fx_check(fx_clean(f, r), m, &r);
            int laps_n = 0, pts_n = 0;
            bool ok = true;
            for (auto& d : g_fx_draws) {                             // (the formatted ones: "<n> <unit>"; the headers are
                if (d.size() < 3 || d[0] < '0' || d[0] > '9') continue;   // drawn from the translations themselves)
                ok &= d.size() <= 255;
                if (d[0] == '5' && d[2] == 'l') { laps_n++; ok &= d == first((std::string("5 ") + la).c_str(), 255); }
                if (d.back() == 'p') pts_n++;
            }
            sprintf(m, "SeasonViewer::Draw, %d- and %d-character translations: every \"<n> <unit>\" 255 characters at most, "
                       "\"5 <Laps>\" cut (%d laps, %d points texts)", n, n + 3, laps_n, pts_n);
            fx_check(ok && laps_n == 3 && pts_n == 3, m);
            o = fx_outside(g_snap, {{(void*)(uintptr_t)S_GX_CANVAS, 4}});
            sprintf(m, "SeasonViewer::Draw, %d- and %d-character translations: nothing written but the current canvas", n, n + 3);
            fx_check(o == 0, m, 0, o);
        }
        setup(mk(0, 'l', 253), "Pts");
        fx_same(f, args, "SeasonViewer::Draw, a laps text of 255 characters");
    }

    // ---- 13. StandingsViewer::Draw: a long Pts translation ----
    {
        const Ent& f = fx_fn("StandingsViewer::Draw");
        FxPatch sd(uit::F_UIStyleDraw, (void*)&fx_UIStyleDraw);
        const std::initializer_list<uint32_t> args = {U(W.standings), 0, U(W.screen)};
        uint8_t* inf = (uint8_t*)(uintptr_t)S_INFO;
        auto setup = [&](const char* pts) {
            fx_reset();
            for (int d = 0; d < 8; d++) {
                *(int32_t*)(inf + 0x170 + 0x88 * d) = d;
                *(int32_t*)(inf + 0x170 + 0x88 * d + 4) = 7;
            }
            UI_GU32(0x005d5d44) = U(pts);
            g_fx_draws.clear();
        };
        for (int n : {254, 600}) {
            const char* pt = mk(0, 'p', n);
            setup(pt);
            mem_save(g_snap);
            r = fx_run(f, true, args);
            sprintf(m, "StandingsViewer::Draw, a %d-character translation: a clean return", n);
            fx_check(fx_clean(f, r), m, &r);
            const std::string e = first((std::string("7 ") + pt).c_str(), 255);
            int k = 0;
            for (auto& d : g_fx_draws) k += d == e;
            sprintf(m, "StandingsViewer::Draw, a %d-character translation: the eight \"7 <Pts>\" texts cut to 255 characters (%d)", n, k);
            fx_check(k == 8, m);
            o = fx_outside(g_snap, {{(void*)(uintptr_t)S_GX_CANVAS, 4}});
            sprintf(m, "StandingsViewer::Draw, a %d-character translation: nothing written but the current canvas", n);
            fx_check(o == 0, m, 0, o);
        }
        setup(mk(0, 'p', 253));
        fx_same(f, args, "StandingsViewer::Draw, texts of 255 characters");
    }

    // ---- 14. driver_compare: drivers' names of 256 or more characters (the points and results equal) ----
    {
        const Ent& f = fx_fn("driver_compare");
        uint8_t* inf = (uint8_t*)(uintptr_t)S_INFO;
        struct { int na, nb; char ta, tb; int want; } cases[] = {
            {300, 300, 'a', 'b', -1}, {300, 20, 'b', 'a', 1}, {20, 600, 'a', 'a', -1}, {400, 400, 'c', 'c', 0},
        };
        for (auto& c : cases) {
            fx_reset();
            for (int d = 0; d < 2; d++) {
                *(int32_t*)(inf + 0x170 + 0x88 * d + 4) = 9;
                for (int k = 0; k < 32; k++) *(int32_t*)(inf + 0x170 + 0x88 * d + 8 + 4 * k) = 3;
            }
            char ta[2] = {c.ta, 0}, tb[2] = {c.tb, 0};
            *(uint32_t*)(g_drivers + 0x210) = U(mk(0, 'n', c.na, ta));
            *(uint32_t*)(g_drivers + 0x220 + 0x210) = U(mk(1, 'n', c.nb, tb));
            W.ints[0] = 0;
            W.ints[1] = 1;
            mem_save(g_snap);
            r = fx_run(f, true, {U(&W.ints[0]), U(&W.ints[1])});
            sprintf(m, "driver_compare, names of %d and %d characters: a clean return, %d (the whole names compared)", c.na + 1,
                    c.nb + 1, c.want);
            fx_check(fx_clean(f, r) && (int32_t)r.ret == c.want, m, &r);
            o = fx_outside(g_snap, {});
            sprintf(m, "driver_compare, names of %d and %d characters: nothing written", c.na + 1, c.nb + 1);
            fx_check(o == 0, m, 0, o);
        }
        fx_reset();
        for (int d = 0; d < 2; d++) {
            *(int32_t*)(inf + 0x170 + 0x88 * d + 4) = 9;
            for (int k = 0; k < 32; k++) *(int32_t*)(inf + 0x170 + 0x88 * d + 8 + 4 * k) = 3;
        }
        *(uint32_t*)(g_drivers + 0x210) = U(mk(0, 'n', 254, "b"));
        *(uint32_t*)(g_drivers + 0x220 + 0x210) = U(mk(1, 'n', 254, "a"));
        W.ints[0] = 0;
        W.ints[1] = 1;
        fx_same(f, {U(&W.ints[0]), U(&W.ints[1])}, "driver_compare, names of 255 characters");
    }

    printf("the fix build: %d directed checks (%d boundary cases on both), %d failed\n", g_fx_n, g_fx_same_n, g_fx_bad);
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
    char* s = strstr(exe, "\\test\\world_career.cpp");
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
        {uit::F_FileOpen, (void*)&stub_FileOpen}, {uit::F_FileClose, (void*)&stub_FileClose},
        {F_ClipboardGetText, (void*)&stub_ClipboardGetText},
        // this group's callees
        {F_vsprintf, (void*)&stub_vsprintf}, {F_stricmp, (void*)&stub_stricmp}, {F_LocaleMoney, (void*)&stub_LocaleMoney},
        {F_LocaleFormatShortDate, (void*)&stub_LocaleFormatShortDate}, {F_OptionsGetI, (void*)&stub_OptionsGetI},
        {F_OptionsGetB, (void*)&stub_OptionsGetB}, {F_OptionsGetS, (void*)&stub_OptionsGetS}, {F_OptionsSetI, (void*)&stub_OptionsSetI},
        {F_FileCreate, (void*)&stub_FileCreate}, {F_FileAppend, (void*)&stub_FileAppend}, {F_FileReadExact, (void*)&stub_FileReadExact},
        {F_FileWrite, (void*)&stub_FileWrite}, {F_FileRemove, (void*)&stub_FileRemove},
        {F_FileCreateDirectory, (void*)&stub_FileCreateDirectory}, {F_Win32GetUserDirectory, (void*)&stub_Win32GetUserDirectory},
        {F_ScanDown, (void*)&stub_ScanDown}, {F_TimeGetTimeOfDay, (void*)&stub_TimeGetTimeOfDay}, {F_Random, (void*)&stub_Random},
        {F_ResourceGet, (void*)&stub_ResourceGet}, {F_ResourceForget, (void*)&stub_ResourceForget}, {F_Xlate, (void*)&stub_Xlate},
        {F_CouldXlate, (void*)&stub_CouldXlate}, {F_AISetDriverMap, (void*)&stub_AISetDriverMap},
        {F_AIGetTrackInfo, (void*)&stub_AIGetTrackInfo}, {F_GetTrackName, (void*)&stub_GetTrackName},
        {F_GetTrackFriendlyName, (void*)&stub_GetTrackFriendlyName}, {F_GetTrackNumber, (void*)&stub_GetTrackNumber},
        {F_GetRealismString, (void*)&stub_GetRealismString}, {F_LoadRace, (void*)&stub_LoadRace}, {F_UnloadRace, (void*)&stub_UnloadRace},
        {F_GenerateCarList, (void*)&stub_GenerateCarList}, {F_PreRaceDo, (void*)&stub_PreRaceDo}, {F_DoRace, (void*)&stub_DoRace},
        {F_CarFileGetUpgradeSet, (void*)&stub_CarFileGetUpgradeSet}, {F_CarFileForgetUpgradeSet, (void*)&stub_CarFileForgetUpgradeSet},
        {F_UIDoOkBox, (void*)&stub_UIDoOkBox}, {F_UIDoYesNoBox, (void*)&stub_UIDoYesNoBox},
        {F_UIDoYesNoCancelBox, (void*)&stub_UIDoYesNoCancelBox},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    g_status_added_orig = *(uint32_t*)(uintptr_t)(VT_CareerStatus + 0xc);
    *(uint32_t*)(uintptr_t)(VT_CareerStatus + 0xc) = U(&hook_status_added);
    patch_gen(std::make_index_sequence<N_GEN>());
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
    const bool cover = getenv("VP_COVER") != 0;
    static const uint32_t k_cover[][2] = {{'PRDO', 0}, {'DORC', 0}, {'FWRT', 0}, {'FRDX', 0}, {'FREM', 0}, {'YNCB', 0}, {'YNBX', 0},
                                          {'OKBX', 0}, {'GEN ', 0x004c5660}, {'GEN ', 0x004c29b0}, {'PANC', 0}, {'RGET', 0},
                                          {'LGET', 0}, {'AISM', 0}, {'GTRF', 0}, {'LMNY', 0}, {'OPTW', 0}, {'RAND', 0}};
    static const char* const k_cover_name[] = {"PreRaceDo", "DoRace", "FileWrite", "FileReadExact", "FileRemove", "YesNoCancel",
                                               "YesNo", "OkBox", "CreditsDo", "UpgradeDo", "LogPanic", "ResourceGet", "Lounge::Get",
                                               "AISetDriverMap", "TrackFriendlyName", "LocaleMoney", "OptionsSet", "Random"};
    enum { N_COVER = sizeof k_cover / sizeof k_cover[0] };
    long long cover_hits[N_COVER];
    long long cover_rounds = 0;
    long long checks = 0, poisoned_checks = 0, log_words = 0;
    int differ = 0, fp_bad = 0, faults = 0, pure_n = 0, replay_n = 0, changed_fns = 0, still_fns = 0, bad_fns = 0;
    long long both_fault = 0;
    int fixed_rounds = 0, fixed_bad = 0;                           // (the fix build: rounds kept out, a fixed case)
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        memset(cover_hits, 0, sizeof cover_hits);
        cover_rounds = 0;
        int fn_faults = 0, fn_checks = 0, fn_fpck = 0, fn_fixed = 0;
        const bool modal = is_modal(f.name);
        int nr = modal ? rounds : rounds * 10;
        if (is_season_end(f.name)) nr *= 4;
        if (f.name[0] == '$') nr = rounds;                   // the $E initialisers: a few stores each
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            if (poisoned) poison(g_pristine);
            g_xl_seed = rnd();
            randomize_world();
            randomize_statics();
            random_script(modal ? 40 : 6);
            uint32_t words[72];
            make_args(f, words);
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            mem_save(g_snap);
            const bool pre_fixed = CAREER_FIXES && fx_pre_case(f, words);
#if CAREER_FIXES
            if (pre_fixed) {                                        // a fixed case: the rewrite clean (or faulting only
                const Result rx = run(f, false, words);             // where the original does, on the round's own
                mem_load(g_snap);                                   // damage: a null season drawn, say)
                const Result rf = run(f, true, words);
                fixed_rounds++;
                fn_fixed++;
                if (rd == 0) { pure_n += fp.pure; replay_n += fp.replay_only != 0; }
                if (rf.fault && !(rx.fault && rx.code == rf.code && rx.eip == rf.eip)) {
                    printf("  %08x %s: round %d, a fixed case: the rewrite faulted (%08x at %08x)\n", f.v10, f.name, rd, rf.code, rf.eip);
                    fixed_bad++;
                    fn_bad = true;
                }
                continue;
            }
#else
            (void)pre_fixed;
#endif
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
            if (cover) {                                   // (VP_COVER: which of the deep callees each round reached)
                for (uint32_t k = 0; k + 1 < g_log.n && k + 1 < LOG_MAX; k++)
                    for (int c = 0; c < N_COVER; c++)
                        if (g_log.w[k] == k_cover[c][0] && (!k_cover[c][1] || g_log.w[k + 1] == k_cover[c][1])) cover_hits[c]++;
                cover_rounds++;
            }
            mem_load(g_snap);
            const Result rn = run(f, true, words);
            checks++;
            fn_checks++;
            poisoned_checks += poisoned;
            log_words += g_log_orig.n;
            if (ro.fault || rn.fault) { faults++; fn_faults++; }
            if (ro.fault && rn.fault) both_fault++;
            if (getenv("VP_DEBUG_LOG")) {
                const uint32_t k0 = getenv("VP_DEBUG_FROM") ? (uint32_t)atoi(getenv("VP_DEBUG_FROM")) : 0u;
                const uint32_t kn = getenv("VP_DEBUG_N") ? (uint32_t)atoi(getenv("VP_DEBUG_N")) : 48u;
                printf("    round %d: %u logged words:", rd, g_log_orig.n);
                for (uint32_t k = k0; k < g_log_orig.n && k < k0 + kn && k < LOG_MAX; k++) {
                    const uint32_t v = g_log_orig.w[k];
                    char c[5];
                    for (int b = 0; b < 4; b++) { const uint8_t ch = (uint8_t)(v >> (24 - 8 * b)); c[b] = ch >= 32 && ch < 127 ? (char)ch : '.'; }
                    c[4] = 0;
                    printf("%s %08x %s", (k - k0) % 8 ? "" : "\n     ", v, c);
                }
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
                    if (getenv("VP_DEBUG_HEAP")) {                  // the heap block it's in (the original's MemAllocs, in order)
                        uint32_t a = ((const HState*)g_pristine.arena)->heap_next;
                        for (uint32_t k = 0; k + 1 < g_log_orig.n && k + 1 < LOG_MAX; k++)
                            if (g_log_orig.w[k] == 'ALOC') {
                                const uint32_t n = g_log_orig.w[k + 1];
                                a = (a + 7) & ~7u;
                                if (where - U(g_arena) >= a && where - U(g_arena) < a + n)
                                    printf("    in the block of 0x%x bytes at arena +%x (+0x%x), allocation at log word %u\n", n, a,
                                           where - U(g_arena) - a, k);
                                a += n;
                            }
                    }
                }
                if (g_log.n != g_log_orig.n) printf("    the stub logs differ: %u / %u words\n", g_log_orig.n, g_log.n);
                for (uint32_t i = 0; i < g_log.n && i < g_log_orig.n && i < LOG_MAX; i++)
                    if (g_log.w[i] != g_log_orig.w[i]) {
                        printf("    the stub logs differ at word %u: %08x / %08x (context:", i, g_log_orig.w[i], g_log.w[i]);
                        const uint32_t back = getenv("VP_DEBUG_CTX") ? (uint32_t)atoi(getenv("VP_DEBUG_CTX")) : 8u;
                        for (uint32_t k = i >= back ? i - back : 0; k < i + 3 && k < g_log.n; k++) printf(" %08x/%08x", g_log_orig.w[k], g_log.w[k]);
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
        if (cover && is_modal(f.name)) {
            printf("  %-28s", f.name);
            for (int c = 0; c < N_COVER; c++) if (cover_hits[c]) printf(" %s %lld", k_cover_name[c], cover_hits[c]);
            printf("  (%lld rounds)\n", cover_rounds);
        }
        if (trace)
            printf("%08x %-52s %s%s (%d checks, %d with the footprint checked, %d faulted)\n", f.v10, f.name, fn_bad ? "BAD" : "ok",
                   fn_changed ? "" : " (never changed memory)", fn_checks, fn_fpck, fn_faults);
        if (fn_fixed) printf("  %08x %s: %d rounds reached a fixed case (kept out of the comparison; the rewrite clean in each, or faulting just where the original did)\n", f.v10,
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
#if CAREER_FIXES
    printf("the fix build: %d rounds reached a fixed case (kept out of the comparison), %d where the rewrite faulted\n", fixed_rounds,
           fixed_bad);
    if (!only) fix_bad = directed_fix_tests();
#else
    (void)fixed_rounds; (void)fixed_bad;
#endif
    return differ || fp_bad || dup || fix_bad || fixed_bad ? 1 : 0;
}
