// world_edit_build.cpp -- M3 UI stage, step U5 (group B): every rewrite of hook/edit_build.cpp and hook/edit_3ds.cpp (library
// `edit`: modbuild.obj, cvt3ds.obj, adtools.obj) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_edit_build.cpp
//        /Fo<dir>\ /Fe<dir>\world_edit_build.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86 /STACK:0x800000
//   run:   world_edit_build.exe [rounds] [seed]        (VP_TRACE=1: one line per function; VP_ONLY=text: those only;
//          VP_DEBUG_WORLD=1: every fault's registers and the footprints of a violation)
//
// Loads out\race_v10.exe at 0x400000 the way test/world_paintkit.cpp does (a child process with the range reserved) and
// includes the two rewrite files with PORT_FN redefined to list each function (its v1.0 address, the rewrite, its calling
// convention, stack arguments and return, its footprint). VP_FAITHFUL (group B has no fixes). It checks that every
// function of the three objects but the $E initialisers (agent A's generated file) is listed exactly once.
//
// The models. Read-only from the test install (..\game-files\installs\v1.0-RC: its .car, .trk and .res resource sets) the
// stock models (MINF resources) of at most 2000 vertices / triangles and 60 surfaces; and generated ones (a few surfaces,
// some textured, NaN / infinite / huge coordinates now and then). The .3ds files: each model written by the ORIGINAL
// Write3DS at start-up (so the reader is fed what the writer writes), and generated chunk trees (materials with colours,
// percentages and maps, objects with vertices, mapping, local axes, faces with their material and smoothing lists,
// unknown chunks, a keyframer section); every chunk's offset kept, so a parser can be handed its own chunk. The DXFs:
// generated 3DFACEs and polyface POLYLINEs, with the errors the reader reports (no polyface flag, counts too low,
// indices of 0 or out of range, a missing VERTEX, quads, junk lines, a file that ends early).
//
// The world, each round: two builders (capacities 40 x 5 and 12 x 3) filled at random but consistent (vertices
// sometimes welded, NaN / infinite; triangles on them with random edge flags; surfaces with texture points and each
// triangle on at most one surface -- as AddTextureTriangle keeps it; their mrModelInfo with random contents within its
// arrays); a model copied in (stock or generated); Read3DS's scratch info (Import3DS's capacities); the files: a .3ds
// (a template, or mutated: bytes flipped, a length changed, cut short), a .dxf, a DXF entity body, out.txt and an export
// file; the statics of the three objects (the reader's and writer's streams -- open on those files at a chunk, or 0 --,
// the chunk stack, the nesting, the 3DS counts, the DXF reader's group, line and values). Half the rounds .data/.bss
// poisoned first (but for the strings this code reads and the C runtime's page). Then for EVERY function, `rounds` times
// (ten times that for the cheap ones): the arguments made for it (the builder and indices in range and out, points and
// floats NaN / inf / huge, chunk ids, a parser's own chunk or a random place, file names that exist or not); the stack
// filled with one pattern after the FPU is set; the ORIGINAL runs from a raw call thunk; its results kept, the snapshot
// restored, the REWRITE runs. Compared: .data/.bss/.idata and the 2 MB arena (the builders, the models, the virtual files'
// bytes, the heap), the return value, the x87 depth, bytes popped, ebx / esi / edi / ebp, faults, and every stub's call
// log. Every byte the original changed must lie in the rewrite's footprint (unless replay_only); a pure one changes no
// global. The x87 at 24 or 53 bits (alternating rounds).
//
// Stubbed (logged; their state in the arena so both passes see the same): MemAlloc / delete (a bump heap in the arena),
// LogReport / LogPanic (the format and its arguments, strings by their text, %f by their bits), the C runtime's stdio as
// a virtual file system in the arena (fopen / fclose / fread / fwrite / ftell / fseek on its files; the FILE's _flag kept,
// EOF at a short read, as the reader tests it; fprintf / printf logged), sprintf, sscanf, strchr, stricmp (the host's,
// logged), the kernel's FileOpen / FileReadLine / FileClose on the same files, mrModelInfoGet (the round's model copy, or
// none) / mrModelInfoForget. The game's own code everywhere else: VectorLength, VectorSub, _VectorNormalize, DotProduct,
// and every function of this group (each rewrite is checked against its original with the same -- original -- callees).
//
// Where the original reads stack garbage, the garbage would move with the frames (a rewritten caller's are other sizes),
// so: a short fread fills what it didn't read with a pattern (the real one leaves it -- a truncated .3ds reads garbage:
// a FIX CANDIDATE); the 16th byte of a triangle made from a copy on the stack (ImportMI, do_3dface, do_polyline and the
// importers over them; never set, never read) is taken as the original's; a pass that logs a million words is ended by
// an exception at the same call in both (a .3ds chunk shorter than its header loops for ever in ADParseChunks: a FIX
// CANDIDATE). The arena has 4 MB of uncommitted space before and after it, so a damaged file's counts fault rather than write here.
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
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif

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

#include "../hook/edit_build.cpp"
#include "../hook/edit_3ds.cpp"

using namespace ebld;

// ---- random numbers --------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x3c6ef372u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int irange(int a, int b) { return b < a ? a : a + (int)(rnd() % (uint32_t)(b - a + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float range(float a, float b) { return a + (b - a) * uni(); }
static float wildf() {
    switch (rnd() % 9) {
    case 0: return bitsf(chance(50) ? 0x7fc00000u : 0xffc00000u);
    case 1: return (rnd() & 1) ? INFINITY : -INFINITY;
    case 2: return chance(50) ? 0.0f : -0.0f;
    case 3: return range(-1e30f, 1e30f);
    case 4: return bitsf(rnd() & 0x807fffff);
    case 5: return bitsf(chance(50) ? 0x7f800001u + (rnd() & 0x3fffff) : 0xff800001u);   // a signalling NaN
    default: return range(-100.0f, 100.0f);
    }
}
static float fval(float lo, float hi) { return chance(10) ? wildf() : range(lo, hi); }

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
enum : uint32_t { A_STATE = 0, A_WORLD = 0x1000, A_HEAP = 0x80000, A_FILES = 0x100000, FILE_CAP = 0x30000 };
static uint8_t* g_arena;
struct Mem { uint8_t* data; uint8_t* idata; uint8_t* arena; };
static Mem g_pristine, g_snap, g_after;
static Mem mem_alloc() { return {(uint8_t*)malloc(DATA_BYTES), (uint8_t*)malloc(IDATA_BYTES), (uint8_t*)malloc(ARENA_BYTES)}; }
static void mem_save(Mem& m) { memcpy(m.data, DATA, DATA_BYTES); memcpy(m.idata, IDATA, IDATA_BYTES); memcpy(m.arena, g_arena, ARENA_BYTES); }
static void mem_load(const Mem& m) { memcpy(DATA, m.data, DATA_BYTES); memcpy(IDATA, m.idata, IDATA_BYTES); memcpy(g_arena, m.arena, ARENA_BYTES); }
static ULONG_PTR g_stack_lo, g_stack_hi;
static bool on_stack(uint32_t a) { return a >= g_stack_lo && a < g_stack_hi; }   // (the part in use: race.exe can lie in the reservation)
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
#define U(p) ((uint32_t)(uintptr_t)(p))

// ---- the stubs' state (in the arena, so each pass starts from the same) ------------------------------------------------------
enum { NFILES = 5, NSTREAMS = 8, NKFD = 4 };
enum { F_3DS, F_DXF, F_BODY, F_OUTTXT, F_EXPORT };
static const char* const k_file_names[NFILES] = {"model.3ds", "model.dxf", "body.dxf", "out.txt", "export.3ds"};
struct VFile { char name[24]; int32_t size, exists, ro; };
struct VStream {                                  // a FILE: _flag at +0xc (0x10: end of file) as the C runtime's
    uint32_t ptr, cnt, base, flag, file, charbuf, bufsiz, tmpfname;
    int32_t vf, pos, open;
};
struct HState {
    uint32_t heap_next;
    VFile files[NFILES];
    VStream streams[NSTREAMS];
    int32_t kvf[NKFD], kpos[NKFD], kopen[NKFD];
    int32_t writes, write_fail_at;              // fwrite: the call that fails (-1: none)
    int32_t wopen_fail;                         // fopen for writing fails
    uint32_t minf;                              // mrModelInfoGet's answer (arena offset; 0: none)
};
#define HS ((HState*)(g_arena + A_STATE))
static uint8_t* file_data(int vf) { return g_arena + A_FILES + (uint32_t)vf * FILE_CAP; }

// ---- the log ----------------------------------------------------------------------------------------------------------------
enum { LOG_MAX = 1 << 18 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
// the watchdog: a pass that logs this much is looping (a .3ds chunk shorter than its header: ADParseChunks seeks back onto
// it for ever, the original as the rewrite) -- it's ended by an exception, at the same call in both passes
enum : uint32_t { WATCHDOG_WORDS = 1000000, WATCHDOG_CODE = 0xe0d0d0d0 };
static void L(uint32_t w) {
    if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w;
    if (++g_log.n == WATCHDOG_WORDS) RaiseException(WATCHDOG_CODE, 0, 0, 0);
}
static uintptr_t g_self_lo, g_self_hi;
static bool readable(const void* p, uint32_t n) {
    const uint64_t a = (uint64_t)(uintptr_t)p, e = a + (uint64_t)n;
    if (a >= (uintptr_t)g_arena && e <= (uint64_t)(uintptr_t)g_arena + ARENA_BYTES) return true;
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
static uint32_t hash_bytes(const void* p, uint32_t n) {
    uint32_t h = 2166136261u;
    if (!readable(p, n ? n : 1)) return 0xbadb10cu;
    for (uint32_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
// a format's arguments: strings by their text, the rest by their words (a %f's two)
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

// ---- stubs: memory, logs ---------------------------------------------------------------------------------------------------
static void* __cdecl stub_MemAlloc(int n) {
    L('ALOC'); L((uint32_t)n);
    uint32_t a = (HS->heap_next + 7) & ~7u;
    if (a < A_HEAP || n < 0 || n > 0x40000 || a + (uint32_t)n > A_FILES) { L('OOM '); return 0; }
    HS->heap_next = a + (uint32_t)n;
    return g_arena + a;
}
static void __cdecl stub_delete(void* p) { L('DEL '); L(P(p)); }
// coverage (VP_COVER=1): how often each message was logged or printed, over the run
static std::vector<std::pair<uint32_t, long long>> g_cover;
static void cover(const void* fmt) {
    for (auto& c : g_cover) if (c.first == U(fmt)) { c.second++; return; }
    g_cover.push_back({U(fmt), 1});
}
static void __cdecl stub_LogReport(const char* fmt, ...) { L('LOGR'); cover(fmt); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); }
static void __cdecl stub_LogPanic(const char* fmt, ...) { L('PANC'); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); }
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }
static int __cdecl stub_crt_msgbox(const char* text, const char* caption, unsigned) {
    printf("the game's CRT would show a message box: %s / %s\n", caption ? caption : "", text ? text : "");
    fflush(stdout);
    ExitProcess(3);
}

// ---- stubs: the C runtime's stdio, a virtual file system in the arena -----------------------------------------------------------
static int find_file(const char* name) {
    if (!name || !readable(name, 1)) return -1;
    for (int i = 0; i < NFILES; i++)
        if (!_stricmp(HS->files[i].name, name)) return i;
    return -1;
}
static VStream* stream_of(void* fp) {
    for (int i = 0; i < NSTREAMS; i++)
        if (fp == &HS->streams[i] && HS->streams[i].open) return &HS->streams[i];
    return 0;
}
static void* __cdecl stub_fopen(const char* name, const char* mode) {
    L('FOPN'); LS(name); LS(mode);
    const int vf = find_file(name);
    if (vf < 0 || !mode || !readable(mode, 1)) return 0;
    VFile& f = HS->files[vf];
    const bool wr = mode[0] == 'w';
    if (!wr && !f.exists) return 0;
    if (wr && (f.ro || HS->wopen_fail)) return 0;
    for (int i = 0; i < NSTREAMS; i++) {
        VStream& s = HS->streams[i];
        if (s.open) continue;
        memset(&s, 0, sizeof s);
        s.vf = vf; s.pos = 0; s.open = 1;
        if (wr) { f.size = 0; f.exists = 1; }
        L(i);
        return &s;
    }
    return 0;
}
static int __cdecl stub_fclose(void* fp) { L('FCLO'); L(P(fp)); VStream* s = stream_of(fp); if (!s) return -1; s->open = 0; return 0; }
static uint32_t __cdecl stub_fread(void* p, uint32_t size, uint32_t n, void* fp) {
    L('FRD '); L(P(p)); L(size); L(n); L(P(fp));
    VStream* s = stream_of(fp);
    if (!s || !size || !n) { L(0); return 0; }
    const uint64_t want = (uint64_t)size * n;
    const int32_t fsz = HS->files[s->vf].size;
    const uint32_t avail = s->pos >= fsz ? 0 : (uint32_t)(fsz - s->pos);
    const uint32_t take = want < avail ? (uint32_t)want : avail;
    if (take) memcpy(p, file_data(s->vf) + s->pos, take);
    s->pos += (int32_t)take;
    if (take < want) s->flag |= 0x10;
    // what a short read leaves unread is filled: the real fread leaves it as it was -- for the reader's locals, the stack's
    // garbage, which moves with the callers' frames (a rewritten caller's are other sizes), so the two passes would read
    // different garbage where the original reads garbage (a truncated .3ds: a FIX CANDIDATE)
    if (take < want && want <= 0x10000) memset((uint8_t*)p + take, 0xa5, (size_t)(want - take));
    L(take / size);
    return take / size;
}
static uint32_t __cdecl stub_fwrite(const void* p, uint32_t size, uint32_t n, void* fp) {
    L('FWR '); L(size); L(n); L(P(fp));
    VStream* s = stream_of(fp);
    const uint64_t want = (uint64_t)size * n;
    if (want <= 0x10000) L(hash_bytes(p, (uint32_t)want)); else L('HUGE');
    if (!s || !size || !n) return 0;
    if (HS->writes++ == HS->write_fail_at) { L('FAIL'); return 0; }
    uint32_t take = want <= 0x10000 ? (uint32_t)want : 0;
    if (s->pos < 0 || (uint32_t)s->pos >= FILE_CAP) take = 0;
    else if (s->pos + take > FILE_CAP) take = FILE_CAP - (uint32_t)s->pos;
    if (take) memcpy(file_data(s->vf) + s->pos, p, take);
    s->pos += (int32_t)take;
    if (s->pos > HS->files[s->vf].size) HS->files[s->vf].size = s->pos;
    return take / size;
}
static int32_t __cdecl stub_ftell(void* fp) { L('FTEL'); L(P(fp)); VStream* s = stream_of(fp); const int32_t r = s ? s->pos : -1; L((uint32_t)r); return r; }
static int32_t __cdecl stub_fseek(void* fp, int32_t off, int32_t whence) {
    L('FSEK'); L(P(fp)); L((uint32_t)off); L((uint32_t)whence);
    VStream* s = stream_of(fp);
    if (!s) return -1;
    const int64_t base = whence == 0 ? 0 : whence == 1 ? s->pos : HS->files[s->vf].size;
    const int64_t np = base + off;
    if (np < 0 || np > 0x7fffffff) return -1;
    s->pos = (int32_t)np;
    s->flag &= ~0x10u;
    return 0;
}
static int __cdecl stub_fprintf(void* fp, const char* fmt, ...) { L('FPRF'); L(P(fp)); va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap); return 0; }
static int __cdecl stub_printf(const char* fmt, ...) {
    L('PRTF');
    cover(fmt);
    if (fmt == (const char*)(uintptr_t)0x004ff404) { L(P(fmt)); return 0; }     // "+%04x FACE LIST [error]\n": no argument given
    va_list ap; va_start(ap, fmt); L_va(fmt, (const uint32_t*)ap); va_end(ap);
    return 0;
}
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    L('SPRF'); L(P(buf)); LS(fmt);
    va_list ap;
    va_start(ap, fmt);
    const int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    LS(buf);
    return r;
}
static int __cdecl stub_sscanf(const char* s, const char* fmt, void* out) {
    L('SSCN'); LS(s); LS(fmt); L(P(out));
    if (!readable(s, 1)) return 0;
    const int r = sscanf(s, fmt, out);
    L((uint32_t)r);
    return r;
}
static char* __cdecl stub_strchr(const char* s, int c) { L('SCHR'); LS(s); L((uint32_t)c); return (char*)strchr(s, c); }
static int __cdecl stub_stricmp(const char* a, const char* b) {
    L('SICM'); LS(a); LS(b);
    const int r = _stricmp(a, b);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
// the kernel's files (the DXF reader): handles 0x100 + slot
static int32_t __cdecl stub_FileOpen(const char* name) {
    L('KOPN'); LS(name);
    const int vf = find_file(name);
    if (vf < 0 || !HS->files[vf].exists) return 0;
    for (int i = 0; i < NKFD; i++)
        if (!HS->kopen[i]) { HS->kopen[i] = 1; HS->kvf[i] = vf; HS->kpos[i] = 0; return 0x100 + i; }
    return 0;
}
static void __cdecl stub_FileClose(int32_t* fd) {
    L('KCLS'); L((uint32_t)*fd);
    const int i = *fd - 0x100;
    if (i >= 0 && i < NKFD) HS->kopen[i] = 0;
    *fd = 0;
}
static uint8_t __cdecl stub_FileReadLine(int32_t fd, char* buf, int32_t n) {
    L('KRDL'); L((uint32_t)fd); L(P(buf)); L((uint32_t)n);
    const int i = fd - 0x100;
    if (i < 0 || i >= NKFD || !HS->kopen[i]) { L(0); return 0; }
    const int32_t size = HS->files[HS->kvf[i]].size;
    const uint8_t* d = file_data(HS->kvf[i]);
    int32_t& pos = HS->kpos[i];
    if (pos >= size) { L(0); return 0; }
    int32_t k = 0;
    while (pos < size && d[pos] != '\n') {
        if (d[pos] != '\r' && k < n - 1) buf[k++] = (char)d[pos];
        pos++;
    }
    if (pos < size) pos++;
    if (n > 0) buf[k] = 0;
    L(1); L(hash_bytes(buf, (uint32_t)k));
    return 1;
}
static void* __cdecl stub_mrModelInfoGet(const char* name) { L('MIGT'); LS(name); return HS->minf ? g_arena + HS->minf : 0; }
static void __cdecl stub_mrModelInfoForget(void* p) { L('MIFG'); L(P(p)); }

// ---- the raw call ------------------------------------------------------------------------------------------------------------------
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
        printf("  fault at %08x: address %08x, eax %08x ecx %08x ebx %08x\n", (uint32_t)e->ContextRecord->Eip,
               (uint32_t)e->ExceptionRecord->ExceptionInformation[1], (uint32_t)e->ContextRecord->Eax, (uint32_t)e->ContextRecord->Ecx,
               (uint32_t)e->ContextRecord->Ebx);
    return EXCEPTION_EXECUTE_HANDLER;
}
// the stack below the call filled with one pattern (Import3DS's frame alone is 0x28840 bytes)
__declspec(noinline) static void fill_stack() {
    volatile uint32_t buf[0x60000 / 4];
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
static uint32_t g_setup_ret;
static void call_orig(uint32_t fn, std::initializer_list<uint32_t> words) {
    Ent e = {fn, "(setup)", 0, 0, (int)words.size(), 4, 0, 0};
    uint32_t w[32];
    int i = 0;
    for (uint32_t x : words) w[i++] = x;
    g_pc = _PC_53;
    const Result r = run(e, false, w);
    g_setup_ret = r.ret;
    if (r.fault) printf("setup: %08x faulted (%08x at %08x)\n", fn, r.code, r.eip);
}

// ---- the models -------------------------------------------------------------------------------------------------------------------
static std::vector<std::vector<uint8_t>> g_models;        // MINF resources: the info (0x28) and its arrays
static int g_stock_models;
static bool minf_ok(const std::vector<uint8_t>& d) {
    if (d.size() < 0x28) return false;
    int32_t c[10];
    memcpy(c, d.data(), 0x28);
    if (c[0] < 0 || c[2] < 0 || c[4] < 0 || c[6] < 0 || c[8] < 0) return false;
    const uint64_t need = 0x28 + 32ull * c[0] + 32ull * c[2] + 8ull * c[4] + 16ull * c[6] + 4ull * c[8];
    return need == d.size() && c[0] <= 2000 && c[4] <= 2000 && c[2] >= 1 && c[2] <= 60;
}
static void load_models(const char* dir) {
    const char* exts[] = {"*.car", "*.trk", "*.res"};
    for (const char* e : exts) {
        char pat[MAX_PATH];
        sprintf(pat, "%s\\%s", dir, e);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            char path[MAX_PATH];
            sprintf(path, "%s\\%s", dir, fd.cFileName);
            FILE* f = fopen(path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> d((size_t)(n > 0 ? n : 0));
            if (n > 0) fread(d.data(), 1, n, f);
            fclose(f);
            if (n < 16 || memcmp(d.data(), "0TSR", 4)) continue;
            int32_t count; memcpy(&count, &d[4], 4);
            size_t off = 16 + 36 * (size_t)count;
            for (int i = 0; i < count && off <= (size_t)n && g_models.size() < 40; i++) {
                uint8_t tag[4]; int32_t size;
                memcpy(tag, &d[16 + 36 * i + 16], 4); memcpy(&size, &d[16 + 36 * i + 24], 4);
                if (size < 0 || off + 8 + size > (size_t)n) break;
                if (!memcmp(tag, "FNIM", 4)) {
                    std::vector<uint8_t> m(d.begin() + off + 8, d.begin() + off + 8 + size);
                    bool dup = false;
                    for (auto& g : g_models) if (g == m) dup = true;
                    if (!dup && minf_ok(m)) g_models.push_back(m);
                }
                off += 8 + size;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    g_stock_models = (int)g_models.size();
}
// a generated model: a few surfaces (some textured), vertices and triangles on them
static std::vector<uint8_t> gen_model() {
    const int ns = irange(1, 4);
    std::vector<int> nv(ns), nt(ns);
    int tv = 0, tt = 0;
    for (int s = 0; s < ns; s++) { nv[s] = irange(0, 12); nt[s] = nv[s] ? irange(0, 14) : 0; tv += nv[s]; tt += nt[s]; }
    std::vector<uint8_t> d(0x28 + 32 * tv + 32 * ns + 8 * tt, 0);
    int32_t c[10] = {tv, 0, ns, 0, tt, 0, 0, 0, 0, 0};
    memcpy(d.data(), c, 0x28);
    float* V = (float*)(d.data() + 0x28);
    for (int i = 0; i < tv * 8; i++) V[i] = (i & 7) < 6 ? fval(-3, 3) : fval(0, 1);
    if (tv > 3 && chance(50)) memcpy(&V[8], &V[0], 32);                          // a duplicate
    uint8_t* S = d.data() + 0x28 + 32 * tv;
    int16_t* T = (int16_t*)(S + 32 * ns);
    int v0 = 0, t0 = 0;
    for (int s = 0; s < ns; s++) {
        uint8_t* r = S + 32 * s;
        if (chance(70)) sprintf((char*)r, chance(20) ? "+bmp%d" : "tex%d.tex", irange(0, 99));
        r[0x10] = (uint8_t)irange(0, 2); r[0x11] = (uint8_t)irange(0, 3);
        int16_t w[6] = {(int16_t)irange(-2, 5), 0, (int16_t)v0, (int16_t)(v0 + nv[s]), (int16_t)t0, (int16_t)(t0 + nt[s])};
        memcpy(r + 0x14, w, 12);
        for (int k = 0; k < nt[s]; k++)
            for (int j = 0; j < 4; j++) T[(t0 + k) * 4 + j] = j < 3 ? (int16_t)(v0 + irange(0, nv[s] - 1)) : (int16_t)irange(0, 6);
        v0 += nv[s]; t0 += nt[s];
    }
    return d;
}
// a copy of model m in the arena at `at`, its pointers made (as mrModelInfoGet makes them)
static MrModelInfo* place_model(const std::vector<uint8_t>& m, uint8_t* at) {
    memcpy(at, m.data(), m.size());
    MrModelInfo* info = (MrModelInfo*)at;
    uint8_t* p = at + 0x28;
    info->verts = info->nverts > 0 ? (MrVertex*)p : 0; p += 32u * (uint32_t)(info->nverts > 0 ? info->nverts : 0);
    info->surfs = info->nsurfs > 0 ? (MrSurface*)p : 0; p += 32u * (uint32_t)(info->nsurfs > 0 ? info->nsurfs : 0);
    info->tris = info->ntris > 0 ? (MrTriangle*)p : 0; p += 8u * (uint32_t)(info->ntris > 0 ? info->ntris : 0);
    info->p1c = info->n18 > 0 ? p : 0; p += 16u * (uint32_t)(info->n18 > 0 ? info->n18 : 0);
    info->p24 = info->n20 > 0 ? p : 0;
    return info;
}

// ---- the .3ds files: written by the original Write3DS, and generated ----------------------------------------------------------------
struct Chunk { uint16_t id; uint32_t at, len; };              // a chunk's header offset and its whole length
struct File3DS { std::vector<uint8_t> d; std::vector<Chunk> chunks; };
static std::vector<File3DS> g_3ds;
static void walk_chunks(File3DS& f, uint32_t at, uint32_t end, int depth) {
    while (at + 6 <= end && depth < 12) {
        uint16_t id; uint32_t len;
        memcpy(&id, &f.d[at], 2); memcpy(&len, &f.d[at + 2], 4);
        if (len < 6 || at + len > end) return;
        f.chunks.push_back({id, at, len});
        uint32_t in = at + 6;
        bool kids = false;
        switch (id) {
        case 0x4d4d: case 0x3d3d: case 0xafff: case 0xa010: case 0xa020: case 0xa030: case 0xa040: case 0xa041: case 0xa042:
        case 0xa050: case 0xa052: case 0xa053: case 0xa084: case 0xa200: case 0xb000: case 0xb002: case 0x4100: kids = true; break;
        case 0x4000:
            while (in < at + len && f.d[in]) in++;
            in++;
            kids = true;
            break;
        case 0x4120: {
            uint16_t n; memcpy(&n, &f.d[in], 2);
            in += 2 + 8u * n;
            kids = true;
            break;
        }
        }
        if (kids && in < at + len) walk_chunks(f, in, at + len, depth + 1);
        at += len;
    }
}
struct CW {                                                    // a chunk writer
    std::vector<uint8_t> d;
    std::vector<size_t> open;
    void b(uint8_t v) { d.push_back(v); }
    void s(int v) { b((uint8_t)v); b((uint8_t)(v >> 8)); }
    void l(uint32_t v) { s((int)(v & 0xffff)); s((int)(v >> 16)); }
    void f(float v) { l(fbits(v)); }
    void str(const char* t) { while (*t) b((uint8_t)*t++); b(0); }
    void begin(int id) { open.push_back(d.size()); s(id); l(0); }
    void end() { const size_t at = open.back(); open.pop_back(); const uint32_t n = (uint32_t)(d.size() - at); memcpy(&d[at + 2], &n, 4); }
};
static const char* k_maps[] = {"wheel.tga", "+sky.bmp", "body", "", "chrome.tex", "averyveryverylongname.tga", "a.b.c"};
static File3DS gen_3ds() {
    CW w;
    w.begin(0x4d4d);
    w.begin(2); w.l(3); w.end();
    w.begin(0x3d3d);
    w.begin(0x3d3e); w.l(3); w.end();
    const int nm = irange(0, 3);
    for (int m = 0; m < nm; m++) {
        w.begin(0xafff);
        w.begin(0xa000); w.str("Mat"); w.end();
        for (int k = 0; k < 3; k++) { w.begin(0xa010 + 0x10 * k); w.begin(0x11); w.b((uint8_t)rnd()); w.b((uint8_t)rnd()); w.b((uint8_t)rnd()); w.end(); w.end(); }
        w.begin(0xa040); w.begin(0x30); w.s(irange(0, 100)); w.end(); w.end();
        if (chance(50)) { w.begin(0xa100); w.s(3); w.end(); }
        if (chance(50)) { w.begin(0xa087); w.f(fval(0, 2)); w.end(); }
        if (chance(30)) { w.begin(0xa0ff); w.l(rnd()); w.end(); }                // unknown
        if (chance(85)) {
            w.begin(0xa200);
            if (chance(90)) { w.begin(0x30); w.s(100); w.end(); }
            if (chance(90)) { w.begin(0xa300); w.str(k_maps[rnd() % 7]); w.end(); }
            if (chance(70)) { w.begin(0xa351); w.s(0); w.end(); }
            if (chance(70)) { w.begin(0xa353); w.f(fval(0, 1)); w.end(); }
            w.end();
        }
        w.end();
    }
    if (chance(70)) { w.begin(0x100); w.f(1.0f); w.end(); }
    const int no = irange(0, 3);
    for (int o = 0; o < no; o++) {
        w.begin(0x4000);
        w.str(chance(90) ? "Obj" : "");
        w.begin(0x4100);
        const int nv = irange(0, 10);
        if (chance(95)) { w.begin(0x4110); w.s(nv); for (int i = 0; i < nv * 3; i++) w.f(fval(-80, 80)); w.end(); }
        if (chance(70)) { w.begin(0x4140); const int n = chance(85) ? nv : irange(0, 12); w.s(n); for (int i = 0; i < n * 2; i++) w.f(fval(0, 1)); w.end(); }
        if (chance(70)) { w.begin(0x4160); for (int i = 0; i < 12; i++) w.f(fval(-1, 1)); w.end(); }
        if (chance(90)) {
            w.begin(0x4120);
            const int nf = nv ? irange(0, 10) : 0;
            w.s(nf);
            for (int i = 0; i < nf; i++) { for (int k = 0; k < 3; k++) w.s(chance(95) ? irange(0, nv - 1) : irange(0, 40)); w.s(6); }
            if (chance(60)) { w.begin(0x4130); w.str("Mat"); w.s(nf); for (int i = 0; i < nf; i++) w.s(i); w.end(); }
            if (chance(50)) { w.begin(0x4150); for (int i = 0; i < nf; i++) w.l(1); w.end(); }
            w.end();
        }
        if (chance(20)) { w.begin(0x4130); w.str("Mat"); w.s(1); w.s(0); w.end(); }
        w.end();
        w.end();
    }
    if (chance(30)) { w.begin(0x1234); w.l(0); w.end(); }
    w.end();
    if (chance(60)) { w.begin(0xb000); w.begin(0xb00a); w.s(5); w.str("MAXSCENE"); w.l(100); w.end(); w.end(); }
    w.end();
    File3DS f;
    f.d = w.d;
    walk_chunks(f, 0, (uint32_t)f.d.size(), 0);
    return f;
}

// ---- the DXFs ------------------------------------------------------------------------------------------------------------------------
static void dx(std::string& s, int code, const std::string& v) { char b[32]; sprintf(b, "%3d\n", code); s += b; s += v; s += chance(50) ? "\n" : "\r\n"; }
static std::string fnum() { char b[48]; float v = fval(-200, 200); if (!isfinite(v)) v = 1e9f; sprintf(b, "%.4f", v); return b; }
static std::string inum(int v) { char b[16]; sprintf(b, "%d", v); return b; }
static void gen_3dface(std::string& s) {
    dx(s, 8, "0");
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 3; k++)
            if (chance(95)) dx(s, 10 * (k + 1) + c, fnum());
    if (chance(20)) dx(s, 62, inum(irange(0, 7)));
}
static void gen_polyline(std::string& s) {
    const int nv = irange(0, 6), nf = irange(0, 5);
    dx(s, 8, "0"); dx(s, 66, "1");
    dx(s, 70, inum(chance(90) ? 64 : 0));
    dx(s, 71, inum(chance(90) ? nv : irange(0, 8)));
    dx(s, 72, inum(chance(90) ? nf : irange(-1, 6)));
    for (int i = 0; i < nv + nf; i++) {
        dx(s, 0, chance(95) ? "VERTEX" : "VERTEZ");
        dx(s, 8, "0");
        if (i < nv) {
            dx(s, 10, fnum()); dx(s, 20, fnum()); dx(s, 30, fnum());
            dx(s, 70, inum(chance(90) ? 192 : 64));
        } else {
            dx(s, 10, "0"); dx(s, 20, "0"); dx(s, 30, "0");
            dx(s, 70, inum(chance(90) ? 128 : 0));
            for (int k = 0; k < 3; k++) dx(s, 71 + k, inum(chance(90) ? (chance(80) && nv ? irange(1, nv) : -irange(0, nv + 1)) : irange(0, 9)));
            if (chance(10)) dx(s, 74, "1");
        }
    }
    dx(s, 0, "SEQEND");
}
static std::string gen_dxf() {
    std::string s;
    dx(s, 0, "SECTION"); dx(s, 2, "ENTITIES");
    const int n = irange(0, 4);
    for (int i = 0; i < n; i++) {
        if (chance(50)) { dx(s, 0, "3DFACE"); gen_3dface(s); }
        else { dx(s, 0, chance(95) ? "POLYLINE" : "polyline"); gen_polyline(s); }
        if (chance(5)) s += "junk\n";
    }
    if (chance(80)) { dx(s, 0, "ENDSEC"); dx(s, 0, "EOF"); }
    if (chance(10)) s.resize(s.size() / 2);
    return s;
}

// ---- the world -------------------------------------------------------------------------------------------------------------------
static uint32_t g_wv;
static void* wv(uint32_t n) { g_wv = (g_wv + 7) & ~7u; void* p = g_arena + g_wv; g_wv += n; memset(p, 0, n); if (g_wv > A_HEAP) { printf("world too big\n"); ExitProcess(2); } return p; }
static char* wstr(const char* s) { char* p = (char*)wv((uint32_t)strlen(s) + 1); strcpy(p, s); return p; }
struct World {
    ModBuilder* mb[2];
    MbPoint* pts[2][8];
    MbTexTri* ttris[2][8];
    uint8_t* model;                                 // the round's model (0x20000 bytes)
    MrModelInfo* rinfo;                             // Read3DS's scratch (Import3DS's capacities)
    MbSurface* dupdst;
    uint8_t* scratch;                               // 0x400 bytes: points, outs
    ADParseBlock* blk;                              // 16 entries
    char* names[8];
    char* longname;
};
static World W;
static const int k_cap[2][2] = {{40, 5}, {12, 3}};   // triangles, surfaces
static void build_world() {
    g_wv = A_WORLD;
    for (int b = 0; b < 2; b++) {
        const int T = k_cap[b][0], S = k_cap[b][1], V = 3 * T;
        ModBuilder* mb = (ModBuilder*)wv(sizeof(ModBuilder));
        mb->maxverts = V; mb->maxtris = T; mb->maxsurfs = S;
        mb->verts = (MbVertex*)wv(V * 0x18);
        mb->tris = (MbTriangle*)wv(T * 0x10);
        mb->surfs = (MbSurface*)wv(S * 0x2c);
        mb->info.verts = (MrVertex*)wv(V * 0x20);
        mb->info.surfs = (MrSurface*)wv((S + 1) * 0x20);
        mb->info.tris = (MrTriangle*)wv(T * 8);
        for (int s = 0; s < S + 1 && s < 8; s++) { W.pts[b][s] = (MbPoint*)wv(V * 0xc); W.ttris[b][s] = (MbTexTri*)wv(T * 0x10); }
        W.mb[b] = mb;
    }
    W.model = (uint8_t*)wv(0x20000);
    W.rinfo = (MrModelInfo*)wv(0x28 + 0x20000 + 0x8000 + 0x800);
    W.dupdst = (MbSurface*)wv(0x2c);
    W.scratch = (uint8_t*)wv(0x400);
    W.blk = (ADParseBlock*)wv(16 * sizeof(ADParseBlock));
    const char* nm[8] = {"model.3ds", "model.dxf", "body.dxf", "out.txt", "export.3ds", "nosuch.3ds", "MODEL.3DS", "model.mod"};
    for (int i = 0; i < 8; i++) W.names[i] = wstr(nm[i]);
    W.longname = wstr("abcdefghijklmnopqrst.tex");
}

// a random builder, consistent: vertices, triangles on them, surfaces with points and texture triangles (each triangle on
// one surface at most), its info's arrays random within their capacities
static void random_builder(int b) {
    ModBuilder* mb = W.mb[b];
    const int T = mb->maxtris, S = mb->maxsurfs, V = mb->maxverts;
    mb->nverts = chance(10) ? V : irange(0, V - 1);
    if (chance(15)) mb->nverts = irange(0, 3);
    for (int i = 0; i < V; i++) {
        MbVertex* v = &mb->verts[i];
        v->x = fval(-5, 5); v->y = fval(-5, 5); v->z = fval(-5, 5);
        v->nx = fval(-1, 1); v->ny = fval(-1, 1); v->nz = fval(-1, 1);
        if (i > 0 && chance(10)) { const MbVertex* o = &mb->verts[irange(0, i - 1)]; v->x = o->x; v->y = o->y; v->z = o->z + (chance(50) ? 0.001f : 0.0f); }
    }
    const int nv = mb->nverts;
    mb->ntris = nv ? (chance(10) ? T : irange(0, T - 1)) : 0;
    for (int i = 0; i < T; i++) {
        MbTriangle* t = &mb->tris[i];
        for (int k = 0; k < 3; k++) t->v[k] = nv ? irange(0, nv - 1) : 0;
        if (nv > 2 && chance(70)) { t->v[0] = irange(0, nv - 1); t->v[1] = (t->v[0] + 1) % nv; t->v[2] = (t->v[0] + 2) % nv; }
        for (int k = 0; k < 3; k++) t->edge[k] = (uint8_t)(chance(90) ? rnd() & 1 : rnd());
        t->pad = (uint8_t)rnd();
    }
    if (mb->ntris > 1 && chance(20)) { const volatile uint32_t* a = (const volatile uint32_t*)&mb->tris[0]; volatile uint32_t* c = (volatile uint32_t*)&mb->tris[1]; c[0] = a[0]; c[1] = a[1]; c[2] = a[2]; }
    mb->nsurfs = irange(0, S);
    // the triangles shuffled, dealt to the surfaces
    int order[64];
    for (int i = 0; i < mb->ntris; i++) order[i] = i;
    for (int i = mb->ntris - 1; i > 0; i--) { const int j = irange(0, i); std::swap(order[i], order[j]); }
    int next = 0;
    for (int s = 0; s < S; s++) {
        MbSurface* sf = &mb->surfs[s];
        memset(sf, 0, sizeof *sf);
        sf->points = W.pts[b][s]; sf->maxpoints = V;
        sf->tris = W.ttris[b][s]; sf->maxtris = T;
        sf->npoints = nv ? (chance(10) ? V : irange(0, nv + 4)) : 0;
        if (chance(8)) sf->npoints = V;
        for (int i = 0; i < V; i++) {
            sf->points[i].vert = nv ? irange(0, nv - 1) : 0;
            sf->points[i].u = chance(30) ? (float)irange(0, 1) : fval(0, 1);
            sf->points[i].v = chance(30) ? (float)irange(0, 1) : fval(0, 1);
        }
        int nt = sf->npoints && s < mb->nsurfs ? irange(0, mb->ntris - next) : 0;
        if (chance(30) && s < mb->nsurfs) nt = mb->ntris - next;
        if (chance(5) && s < mb->nsurfs) nt = T;
        if (nt > T) nt = T;
        for (int i = 0; i < T; i++) {
            MbTexTri* tt = &sf->tris[i];
            for (int k = 0; k < 3; k++) tt->p[k] = sf->npoints ? irange(0, sf->npoints - 1) : 0;
            tt->tri = next + i < mb->ntris ? order[next + i] : irange(mb->ntris, T + 3);
        }
        if (nt > mb->ntris - next) nt = mb->ntris - next;
        if (nt < 0) nt = 0;
        sf->ntris = nt;
        next += nt;
        if (chance(80)) sprintf(sf->name, "%s%d", chance(20) ? "+" : "t", irange(0, 999));
        sf->mat0 = (uint8_t)rnd(); sf->mat1 = (uint8_t)rnd(); sf->group = (int16_t)rnd();
    }
    // the info: random contents, counts within the arrays
    for (uint32_t i = 0; i < (uint32_t)V * 8; i++) ((volatile float*)mb->info.verts)[i] = fval(-2, 2);
    for (uint32_t i = 0; i < (uint32_t)(S + 1) * 0x20; i++) ((uint8_t*)mb->info.surfs)[i] = (uint8_t)rnd();
    for (int i = 0; i < T; i++) for (int k = 0; k < 4; k++) mb->info.tris[i].v[k] = (int16_t)irange(0, V - 1);
    mb->info.nsurfs = irange(1, S);
    for (int s = 0; s <= S; s++) {
        MrSurface* r = &mb->info.surfs[s];
        r->name[irange(0, 15)] = 0;
        const int a = irange(0, V - 1), c = irange(0, V - 1);
        r->v0 = (int16_t)(chance(85) ? (a < c ? a : c) : (a > c ? a : c)); r->v1 = (int16_t)(a < c ? c : a);
        const int x = irange(0, T - 1), y = irange(0, T - 1);
        r->t0 = (int16_t)(x < y ? x : y); r->t1 = (int16_t)(x < y ? y : x);
    }
    mb->info.nverts = irange(0, V);
    mb->info.ntris = irange(0, T);
}

// the files: a .3ds (a template or generated, mutated now and then), a .dxf, a DXF entity body, out.txt, the export
static int g_3ds_pick;
static void random_files() {
    memset(HS->files, 0, sizeof HS->files);
    memset(HS->streams, 0, sizeof HS->streams);
    memset(HS->kopen, 0, sizeof HS->kopen);
    for (int i = 0; i < NFILES; i++) { strcpy(HS->files[i].name, k_file_names[i]); HS->files[i].exists = 1; }
    g_3ds_pick = (int)(rnd() % g_3ds.size());
    const File3DS& t = g_3ds[g_3ds_pick];
    uint32_t n = (uint32_t)t.d.size() < FILE_CAP ? (uint32_t)t.d.size() : FILE_CAP;
    memcpy(file_data(F_3DS), t.d.data(), n);
    if (chance(20) && n) {                                          // mutated
        const int k = irange(1, 4);
        for (int i = 0; i < k; i++) file_data(F_3DS)[rnd() % n] ^= (uint8_t)(1u << (rnd() & 7));
        if (chance(30)) n = (uint32_t)irange(0, (int)n);
        if (chance(20) && !t.chunks.empty()) {
            const Chunk& c = t.chunks[rnd() % t.chunks.size()];
            const uint32_t len = c.len + (uint32_t)irange(-8, 8);
            memcpy(file_data(F_3DS) + c.at + 2, &len, 4);
        }
    }
    HS->files[F_3DS].size = (int32_t)n;
    const std::string dxf = gen_dxf();
    memcpy(file_data(F_DXF), dxf.data(), dxf.size());
    HS->files[F_DXF].size = (int32_t)dxf.size();
    std::string body;
    if (chance(50)) gen_3dface(body); else gen_polyline(body);
    if (chance(80)) dx(body, 0, "EOF");
    memcpy(file_data(F_BODY), body.data(), body.size());
    HS->files[F_BODY].size = (int32_t)body.size();
    HS->files[F_OUTTXT].size = 0;
    const int ex = irange(0, 200);
    for (int i = 0; i < ex; i++) file_data(F_EXPORT)[i] = (uint8_t)rnd();
    HS->files[F_EXPORT].size = ex;
    HS->files[F_EXPORT].ro = chance(5);
    if (chance(5)) HS->files[F_3DS].exists = 0;
    if (chance(5)) HS->files[F_DXF].exists = 0;
    HS->writes = 0;
    HS->write_fail_at = chance(10) ? irange(0, 40) : -1;
    HS->wopen_fail = chance(3);
}
static VStream* open_stream(int vf, int32_t pos) {
    for (int i = 0; i < NSTREAMS; i++) {
        VStream& s = HS->streams[i];
        if (s.open) continue;
        memset(&s, 0, sizeof s);
        s.vf = vf; s.pos = pos; s.open = 1;
        return &s;
    }
    return 0;
}
static int32_t open_kfd(int vf) {
    for (int i = 0; i < NKFD; i++)
        if (!HS->kopen[i]) { HS->kopen[i] = 1; HS->kvf[i] = vf; HS->kpos[i] = 0; return 0x100 + i; }
    return 0;
}
// a chunk of the round's .3ds with this id (or any), as (its header, its length); false if none
static bool pick_chunk(int id, Chunk* out) {
    const File3DS& t = g_3ds[g_3ds_pick];
    int cands[256], n = 0;
    for (size_t i = 0; i < t.chunks.size() && n < 256; i++)
        if (id < 0 || t.chunks[i].id == id) cands[n++] = (int)i;
    if (!n) return false;
    *out = t.chunks[cands[rnd() % n]];
    return true;
}
// the reader: open on the .3ds at a chunk's data (or anywhere), the limit at its end; the log open or not
static int32_t g_chunk_len;
static void reader_at(int id) {
    Chunk c;
    int32_t pos = 0, limit = 100000000;
    g_chunk_len = irange(0, 64);
    if (chance(85) && pick_chunk(id, &c)) { pos = (int32_t)c.at + 6; limit = (int32_t)(c.at + c.len); g_chunk_len = (int32_t)c.len - 6; }
    else if (chance(50) && pick_chunk(-1, &c)) { pos = (int32_t)c.at + (chance(50) ? 0 : 6); limit = (int32_t)(c.at + c.len); }
    else pos = irange(0, HS->files[F_3DS].size + 4);
    if (chance(10)) limit = irange(0, HS->files[F_3DS].size + 10);
    EB_GP(void, S_AD_IN) = chance(97) ? (void*)open_stream(F_3DS, pos) : 0;
    EB_GP(void, S_AD_LOG) = chance(50) ? (void*)open_stream(F_OUTTXT, 0) : 0;
    EB_G32(S_AD_LIMIT) = limit;
    EB_G32(S_AD_NEST) = irange(0, 4);
}
static void writer_open() {
    EB_GP(void, S_AD_OUT) = chance(97) ? (void*)open_stream(F_EXPORT, irange(0, HS->files[F_EXPORT].size)) : 0;
    const int d = irange(0, 5);
    EB_G32(S_AD_DEPTH) = d;
    for (int i = 0; i < d; i++) EB_G32(S_AD_STACK + 4 * i) = irange(0, HS->files[F_EXPORT].size);
}
static MrModelInfo* g_model;
static void random_world() {
    random_builder(0);
    random_builder(1);
    const int pick = chance(60) && g_stock_models ? (int)(rnd() % g_stock_models) : -1;
    const std::vector<uint8_t> m = pick >= 0 ? g_models[pick] : gen_model();
    g_model = place_model(m.size() <= 0x20000 ? m : gen_model(), W.model);
    HS->minf = chance(90) ? (uint32_t)(W.model - g_arena) : 0;
    MrModelInfo* r = W.rinfo;
    memset(r, 0, 0x28);
    r->verts = (MrVertex*)((uint8_t*)r + 0x28);
    r->tris = (MrTriangle*)((uint8_t*)r + 0x28 + 0x20000);
    r->surfs = (MrSurface*)((uint8_t*)r + 0x28 + 0x28000);
    for (int i = 0; i < 0x400; i++) W.scratch[i] = (uint8_t)rnd();
    HS->heap_next = A_HEAP;
    random_files();
}
// .data after poisoning, and each round: the statics of this group
static void randomize_statics() {
    EB_G32(S_3DS_NVERTS) = irange(0, 20);
    EB_G32(S_3DS_NTRIS) = irange(0, 20);
    EB_G32(S_3DS_NMATS) = irange(0, 6);
    EB_G32(S_3DS_NOBJS) = irange(0, 6);
    EB_G32(S_3DS_NUV) = irange(0, 20);
    for (int i = 0; i < 12; i++) EB_GF(S_3DS_FRAME + 4 * i) = chance(70) ? ((i % 4 == 0 && i < 9) ? 1.0f : 0.0f) : fval(-2, 2);
    EB_G32(S_DXF_CODE) = chance(50) ? -1 : irange(-1, 80);
    const char* lines[] = {"3DFACE", "POLYLINE", "VERTEX", "12.5", "-3", "", "SEQEND"};
    strcpy((char*)(uintptr_t)S_DXF_LINE, lines[rnd() % 7]);
    strcpy((char*)(uintptr_t)S_DXF_GROUP, lines[rnd() % 7]);
    EB_G32(S_DXF_INT) = irange(-5, 200);
    EB_GF(S_DXF_FLOAT) = fval(-100, 100);
    EB_G8(S_FACE_WARNED) = (uint8_t)(rnd() & 1);
    EB_GP(void, S_AD_IN) = 0; EB_GP(void, S_AD_OUT) = 0; EB_GP(void, S_AD_LOG) = 0;
    EB_G32(S_AD_LIMIT) = 100000000; EB_G32(S_AD_NEST) = 0; EB_G32(S_AD_DEPTH) = 0;
}

// ---- poison: .data/.bss random but for the strings this code reads and the C runtime's page -------------------------------------
static void poison(const Mem& keepfrom) {
    uint32_t* d = (uint32_t*)DATA;
    for (uint32_t i = 0; i < DATA_BYTES / 4; i++) d[i] = rnd();
    static const uint32_t keep[][2] = {
        {0x004fefe0, 0x004ff600},             // modbuild / cvt3ds / adtools: their strings
        {0x00502000, 0x00503000},             // the C runtime's own data (__adjust_fdiv)
    };
    for (auto& k : keep) memcpy(DATA + (k[0] - 0x004e1000), keepfrom.data + (k[0] - 0x004e1000), k[1] - k[0]);
}

// ---- arguments -------------------------------------------------------------------------------------------------------------------
static uint32_t idx(int n, int extra = 2) { return (uint32_t)(chance(90) ? irange(0, n > 0 ? n - 1 : 0) : irange(-1, n + extra)); }
static uint32_t pt3(float lo, float hi) {                      // a point in the scratch
    float* p = (float*)(W.scratch + 0x200 + 16 * (rnd() % 8));
    for (int k = 0; k < 3; k++) p[k] = fval(lo, hi);
    return U(p);
}
static const uint32_t k_parsers[] = {F_parse_primary, F_parse_m3d_version, F_parse_edit, F_parse_mesh_version, F_parse_material_list,
                                     F_parse_materials_name, F_parse_texture, F_parse_object, F_parse_tri_list, F_parse_vertex_list,
                                     F_parse_face_list, F_parse_face_material, F_parse_mapping_coords, F_parse_mesh_mat_group,
                                     F_parse_smooth_group, F_parse_local_axis, F_parse_material_color, F_parse_material_percent,
                                     F_parse_color, F_parse_percent, F_parse_short, F_parse_float};
static const uint16_t k_parser_id[] = {0x4d4d, 0x0002, 0x3d3d, 0x3d3e, 0xafff, 0xa000, 0xa200, 0x4000, 0x4100, 0x4110, 0x4120,
                                       0x4130, 0x4140, 0x4130, 0x4150, 0x4160, 0xa010, 0xa040, 0x0011, 0x0030, 0xa100, 0x0100};
static bool make_args(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 64; i++) w[i] = rnd();
    const uint32_t a = f.v10;
    ModBuilder* mb = W.mb[chance(70) ? 0 : 1];
    const uint32_t MB = U(mb);
    uint8_t* sc = W.scratch;
    // the parsers: (id, the chunk's data length, the info) with the reader on their own chunk
    for (size_t i = 0; i < sizeof k_parsers / 4; i++)
        if (a == k_parsers[i]) {
            reader_at(chance(90) ? k_parser_id[i] : -1);
            w[0] = (rnd() & 0xffff0000u) | k_parser_id[i];
            w[1] = (uint32_t)g_chunk_len;
            w[2] = U(W.rinfo);
            return true;
        }
    switch (a) {
    // ---- adtools
    case F_ADParseChunks: {
        reader_at(-1);
        const int n = irange(0, 6);
        for (int i = 0; i < n; i++) {
            const int k = (int)(rnd() % (sizeof k_parsers / 4));
            W.blk[i].id = chance(10) ? 0xffff : chance(80) ? k_parser_id[k] : (uint16_t)rnd() | 1;
            W.blk[i].fn = chance(90) ? (ADParser)(uintptr_t)k_parsers[k] : 0;
            W.blk[i].data = W.rinfo;
        }
        W.blk[n].id = 0; W.blk[n].fn = 0; W.blk[n].data = 0;
        w[0] = chance(95) ? U(W.blk) : 0;
        return true;
    }
    case F_out_tab: case 0x004bbfb0: case F_ADClose: reader_at(-1); return true;
    case F_ADOpen: case F_ADCreate: w[0] = U(W.names[rnd() % 8]); return true;
    case F_ADReadChunk: reader_at(-1); { Chunk c; w[0] = (rnd() & 0xffff0000u) | (chance(70) && pick_chunk(-1, &c) ? c.id : (rnd() & 0xffff)); } w[1] = chance(80) ? U(sc) : 0; return true;
    case F_ADRead: reader_at(-1); w[0] = U(sc); w[1] = (uint32_t)irange(-1, 0x60); return true;
    case F_ADReadString: reader_at(-1); w[0] = U(sc); w[1] = (uint32_t)irange(-1, 0x40); return true;
    case F_ADReadShort: case F_ADReadLong: case F_ADReadFloat: case F_ADReadChar: reader_at(-1); w[0] = U(sc + irange(0, 8)); return true;
    case 0x004bbf60: reader_at(-1); w[0] = (uint32_t)irange(-20, 100); return true;                        // ADSkip
    case F_ADCreateClose: case F_ADCloseChunk: writer_open(); if (chance(10)) EB_G32(S_AD_DEPTH) = 0; return true;
    case F_ADCreateChunk: writer_open(); w[0] = rnd(); return true;
    case F_ADWriteShort: case F_ADWriteLong: case F_ADWriteFloat: case F_ADWriteByte: writer_open(); return true;
    case F_ADWriteString: writer_open(); w[0] = chance(80) ? U(W.names[rnd() % 8]) : U(W.longname); return true;
    case F_ADWrite: writer_open(); w[0] = U(sc); w[1] = (uint32_t)irange(0, 0x60); return true;
    // ---- cvt3ds
    case F_Read3DS: w[0] = U(W.rinfo); w[1] = U(W.names[chance(85) ? 0 : 5]); return true;
    case 0x004ba440: w[0] = U(chance(85) ? g_model : &mb->info); w[1] = U(W.names[chance(90) ? 4 : 5]); return true;   // Write3DS
    case F_ADWriteColorChunk: writer_open(); w[0] = rnd() & 0x1ff; w[1] = rnd() & 0x1ff; w[2] = rnd() & 0x1ff; return true;
    case F_ADWritePercentageChunk: writer_open(); return true;
    case F_fixup_vertex: {
        float* v = (float*)sc;
        for (int k = 0; k < 8; k++) v[k] = fval(-100, 100);
        float* fr = (float*)(sc + 0x40);
        for (int k = 0; k < 12; k++) fr[k] = chance(30) ? ((k % 4 == 0 && k < 9) ? 1.0f : 0.0f) : fval(-2, 2);
        w[0] = U(v); w[1] = chance(30) ? (uint32_t)S_3DS_FRAME : U(fr);
        return true;
    }
    // ---- modbuild
    case 0x004b7050: w[0] = (uint32_t)irange(0, 20); w[1] = (uint32_t)irange(0, 5); return true;                // ModBuilderCreate
    case 0x004b7140: case F_ModBuilderClear: case 0x004b75b0: case 0x004b7690: case 0x004b7dc0: case F_ModBuilderAddSurface:
    case 0x004b7dd0: case F_make_vertex_normals:
        w[0] = MB; return true;
    case 0x004b7220: w[0] = U(W.mb[0]); w[1] = U(W.mb[1]); if (chance(30)) w[1] = U(W.mb[0] == mb ? W.mb[0] : W.mb[0]); return true;  // Copy
    case F_dup_surface: w[0] = U(W.dupdst); w[1] = U(&mb->surfs[irange(0, mb->maxsurfs - 1)]); return true;
    case F_ModBuilderAddVertex: {
        float* p = (float*)(sc + 0x200);
        if (mb->nverts && chance(50)) { const MbVertex* v = &mb->verts[irange(0, mb->nverts - 1)]; p[0] = v->x + (chance(50) ? 0.003f : 0); p[1] = v->y; p[2] = v->z; }
        else for (int k = 0; k < 3; k++) p[k] = fval(-5, 5);
        w[0] = MB; w[1] = U(p);
        return true;
    }
    case 0x004b7430: w[0] = MB; w[1] = idx(mb->nverts); w[2] = U(sc + 0x100); return true;                     // GetVertex
    case F_ModBuilderAddTriangle: case 0x004b7560: {                                                           // Add / SetTriangle
        uint32_t* t = (uint32_t*)(sc + 0x180);
        if (mb->ntris && chance(40)) memcpy(t, &mb->tris[irange(0, mb->ntris - 1)], 16);
        else { for (int k = 0; k < 3; k++) t[k] = idx(mb->nverts); t[3] = rnd(); }
        w[0] = MB;
        if (a == F_ModBuilderAddTriangle) w[1] = U(t); else { w[1] = idx(mb->ntris); w[2] = U(t); }
        return true;
    }
    case F_is_triangle_same: {
        uint32_t* t = (uint32_t*)(sc + 0x180);
        for (int k = 0; k < 8; k++) t[k] = (uint32_t)irange(0, 3);
        w[0] = U(t); w[1] = U(t + 4);
        return true;
    }
    case 0x004b7520: w[0] = MB; w[1] = idx(mb->ntris); w[2] = U(sc + 0x100); return true;                      // GetTriangle
    case 0x004b75f0: case 0x004b76d0: {                                                                       // Smooth / FacetEdge
        w[0] = MB;
        if (mb->ntris && chance(70)) { const MbTriangle* t = &mb->tris[irange(0, mb->ntris - 1)]; const int k = irange(0, 2);
            w[1] = (uint32_t)t->v[k]; w[2] = (uint32_t)t->v[(k + 1) % 3]; if (chance(50)) std::swap(w[1], w[2]); }
        else { w[1] = idx(mb->nverts); w[2] = idx(mb->nverts); }
        return true;
    }
    case 0x004b7770: case 0x004b77b0: w[0] = MB; w[1] = pt3(-3, 3); return true;                               // Translate / Scale
    case 0x004b78c0: w[0] = MB; w[1] = idx(mb->nsurfs); return true;                                           // IsSurface
    case F_ModBuilderSetSurfaceTexture: w[0] = MB; w[1] = (uint32_t)irange(0, mb->maxsurfs - 1); w[2] = chance(85) ? U(W.names[rnd() % 8]) : U(W.longname); return true;
    case 0x004b7920: w[0] = MB; w[1] = (uint32_t)irange(0, mb->maxsurfs - 1); w[2] = U(sc + 0x100); return true;   // GetSurfaceTexture
    case F_ModBuilderSetSurfaceMaterial: w[0] = MB; w[1] = (uint32_t)irange(0, mb->maxsurfs - 1); return true;
    case 0x004b7990: w[0] = MB; w[1] = (uint32_t)irange(0, mb->maxsurfs - 1); w[2] = U(sc + 0x100); w[3] = U(sc + 0x101); return true;
    case F_ModBuilderSetSurfaceGroup: w[0] = MB; w[1] = (uint32_t)irange(0, mb->maxsurfs - 1); return true;
    case 0x004b79e0: w[0] = MB; w[1] = (uint32_t)irange(0, mb->maxsurfs - 1); w[2] = U(sc + 0x102); return true;
    case F_ModBuilderAddTextureTriangle: {
        const int s = irange(0, mb->maxsurfs - 1);
        w[0] = MB; w[1] = (uint32_t)s; w[2] = idx(mb->ntris);
        for (int k = 0; k < 3; k++) w[3 + k] = idx(mb->surfs[s].npoints);
        if (chance(10)) mb->surfs[s].ntris = mb->surfs[s].maxtris;
        return true;
    }
    case 0x004b7ae0: { const int s = irange(0, mb->maxsurfs - 1); w[0] = MB; w[1] = (uint32_t)s; w[2] = idx(mb->surfs[s].ntris);
        for (int k = 0; k < 4; k++) w[3 + k] = U(sc + 0x100 + 4 * k); return true; }
    case 0x004b7b40: { const int s = irange(0, mb->maxsurfs - 1); w[0] = MB; w[1] = (uint32_t)s; w[2] = idx(mb->surfs[s].ntris); return true; }
    case F_ModBuilderAddTexturePoint: {
        const int s = irange(0, mb->maxsurfs - 1);
        MbSurface* S = &mb->surfs[s];
        w[0] = MB; w[1] = (uint32_t)s;
        if (S->npoints && chance(50)) { const MbPoint* p = &S->points[irange(0, S->npoints - 1)]; w[2] = (uint32_t)p->vert;
            w[3] = fbits(p->u + (chance(30) ? 1e-8f : 0.0f)); w[4] = fbits(p->v); }
        else { w[2] = idx(mb->nverts); w[3] = fbits(fval(0, 1)); w[4] = fbits(fval(0, 1)); }
        if (chance(10)) S->npoints = S->maxpoints;
        return true;
    }
    case 0x004b7c70: { const int s = irange(0, mb->maxsurfs - 1); w[0] = MB; w[1] = (uint32_t)s; w[2] = idx(mb->surfs[s].npoints);
        for (int k = 0; k < 3; k++) w[3 + k] = U(sc + 0x100 + 4 * k); return true; }
    case 0x004b7cc0: { const int s = irange(0, mb->maxsurfs - 1); w[0] = MB; w[1] = (uint32_t)s; w[2] = idx(mb->surfs[s].npoints);
        if (chance(30)) mb->surfs[s].ntris = 0; return true; }
    case 0x004b7d90: { const int s = irange(0, mb->maxsurfs - 1); w[0] = MB; w[1] = (uint32_t)s;
        w[2] = (uint32_t)irange(0, mb->surfs[s].maxpoints - 1); w[3] = fbits(fval(0, 1)); w[4] = fbits(fval(0, 1)); return true; }
    case F_info_add_surface: w[0] = U(&mb->info); w[1] = chance(80) ? U(W.names[rnd() % 8]) : U(W.longname);
        w[2] = rnd(); w[3] = rnd(); w[4] = rnd(); mb->info.nsurfs = irange(0, mb->maxsurfs); return true;
    case F_info_add_vertex: {
        w[0] = U(&mb->info);
        MrSurface* S = &mb->info.surfs[mb->info.nsurfs - 1];
        if (S->v1 > S->v0 && chance(50)) {
            const MrVertex* v = &mb->info.verts[irange(S->v0, S->v1 - 1)];
            float* p = (float*)(sc + 0x200);
            memcpy(p, v, 32);
            if (chance(30)) p[irange(0, 7)] = fval(-2, 2);
            w[1] = U(p); w[2] = U(p + 3); w[3] = fbits(p[6]); w[4] = fbits(p[7]);
        } else { w[1] = pt3(-2, 2); w[2] = pt3(-1, 1); w[3] = fbits(fval(0, 1)); w[4] = fbits(fval(0, 1)); }
        if (S->v1 >= mb->maxverts - 1) S->v1 = (int16_t)(mb->maxverts - 2);
        if (S->v0 >= mb->maxverts - 1) S->v0 = (int16_t)(mb->maxverts - 2);
        return true;
    }
    case F_info_add_triangle: { w[0] = U(&mb->info); w[1] = rnd(); w[2] = rnd(); w[3] = rnd();
        MrSurface* S = &mb->info.surfs[mb->info.nsurfs - 1]; if (S->t1 >= mb->maxtris) S->t1 = (int16_t)(mb->maxtris - 1); return true; }
    case 0x004b8bd0: w[0] = MB; w[1] = chance(85) ? U(W.names[rnd() % 8]) : U(W.longname); return true;        // BuildGeometry
    case 0x004b8e10: w[0] = U(&mb->info); return true;                                                         // DeselectAll
    case 0x004b8e90: w[0] = U(&mb->info); w[1] = idx(mb->info.ntris); w[2] = (uint32_t)irange(-1, 3); return true;
    case 0x004b8fa0: w[0] = MB; w[1] = U(W.names[chance(90) ? 1 : 5]); w[2] = rnd() & 0x1ff; return true;      // ImportDXF
    case F_do_3dface: case F_do_polyline: w[0] = MB; w[1] = (uint32_t)open_kfd(F_BODY); if (chance(50)) EB_G32(S_DXF_CODE) = -1; return true;
    case F_make_vertex: w[0] = U(sc + 0x100); w[1] = fbits(fval(-500, 500)); w[2] = fbits(fval(-500, 500)); w[3] = fbits(fval(-500, 500)); return true;
    case F_ModBuilderImportMI: w[0] = MB; w[1] = U(g_model); return true;
    case 0x004b9800: w[0] = MB; w[1] = U(W.names[7]); return true;                                             // ImportMOD
    case 0x004b9890: {                                                                                        // Import3DS
        // the .3ds as written (never mutated): a damaged count overruns Import3DS's frame (the FIX CANDIDATE), which would
        // overwrite this program's own stack above it
        const File3DS& t = g_3ds[g_3ds_pick];
        memcpy(file_data(F_3DS), t.d.data(), t.d.size());
        HS->files[F_3DS].size = (int32_t)t.d.size();
        w[0] = MB; w[1] = U(W.names[chance(90) ? 0 : 5]);
        return true;
    }
    case F_get_face_normal: w[0] = MB; w[1] = U(sc + 0x100); w[2] = idx(mb->ntris); return true;
    case F_edge_compare: for (int k = 0; k < 4; k++) w[k] = (uint32_t)irange(0, 3); return true;
    case F_dxf_next_group: case F_dxf_read_item: case F_dxf_next_item: {
        const int32_t fd = open_kfd(chance(50) ? F_DXF : F_BODY);
        if (fd && chance(60)) HS->kpos[fd - 0x100] = irange(0, HS->files[HS->kvf[fd - 0x100]].size);
        w[0] = chance(95) ? (uint32_t)fd : 0;
        return true;
    }
    case F_ModBuilderVertex_ctor: w[0] = U(sc); return true;
    }
    return false;
}
static bool is_slow(uint32_t a) {
    return a == F_Read3DS || a == 0x004ba440 || a == 0x004b9890 || a == 0x004b95a0 || a == 0x004b9800 || a == 0x004b8fa0 ||
           a == 0x004b7dd0 || a == F_make_vertex_normals || a == F_parse_primary || a == F_parse_edit || a == F_ADParseChunks;
}

// ---- the inventory: every function of the three objects, but the $E initialisers -----------------------------------------------
static std::vector<std::pair<uint32_t, std::string>> g_inv;
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
        if (strcmp(c[3], "modbuild.obj") && strcmp(c[3], "cvt3ds.obj") && strcmp(c[3], "adtools.obj")) continue;
        if (c[5][0] == '$' && c[5][1] == 'E') continue;
        g_inv.push_back({(uint32_t)strtoul(c[0], 0, 16), c[4]});
    }
    fclose(f);
}

// an exception nothing caught: where, and in which check (VP_DEBUG_WORLD prints every fault as it's caught)
static const char* g_cur_fn = "(setup)";
static int g_cur_round = -1, g_cur_pass = -1;
static LONG WINAPI unhandled(EXCEPTION_POINTERS* e) {
    printf("UNCAUGHT exception %08x at %08x (address %08x) in %s, round %d, pass %d\n", (uint32_t)e->ExceptionRecord->ExceptionCode,
           (uint32_t)e->ContextRecord->Eip, (uint32_t)e->ExceptionRecord->ExceptionInformation[1], g_cur_fn, g_cur_round, g_cur_pass);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
// ---- main -----------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    SetUnhandledExceptionFilter(unhandled);
    setvbuf(stdout, 0, _IONBF, 0);
    const int rounds = argc > 1 ? atoi(argv[1]) : 24;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    const char* only = getenv("VP_ONLY");
    const bool step = getenv("VP_STEP") != 0;
    char root[MAX_PATH], exe[MAX_PATH], inv[MAX_PATH], install[MAX_PATH];
    strcpy(root, __FILE__);
    char* s = strstr(root, "\\test\\world_edit_build.cpp");
    if (s) *s = 0;
    sprintf(inv, "%s\\out\\inventory.csv", root);
    sprintf(exe, "%s\\out\\race_v10.exe", root);
    sprintf(install, "%s\\..\\game-files\\installs\\v1.0-RC", root);
    if (!load_race_exe(exe)) return 2;
    read_inventory(inv);
    GetCurrentThreadStackLimits(&g_stack_lo, &g_stack_hi);
    {
        volatile uint8_t here;
        const ULONG_PTR lo = (ULONG_PTR)&here - 0x200000;           // the stack these tests use (with the image above it)
        if (lo > g_stack_lo) g_stack_lo = lo;
    }
    {
        HMODULE m = GetModuleHandleA(0);
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
        g_self_lo = (uintptr_t)m; g_self_hi = (uintptr_t)m + nt->OptionalHeader.SizeOfImage;
    }
    struct { uint32_t at; void* to; } stubs[] = {
        {0x004cf730, (void*)&stub_crt_fatal}, {0x004d45b0, (void*)&stub_crt_fatal}, {0x004d4570, (void*)&stub_crt_fatal},
        {0x004d5c10, (void*)&stub_crt_fatal}, {0x004d6850, (void*)&stub_crt_msgbox},
        {F_MemAlloc, (void*)&stub_MemAlloc}, {F_Delete, (void*)&stub_delete}, {F_LogReport, (void*)&stub_LogReport},
        {0x004112b0, (void*)&stub_LogPanic},
        {F_fopen, (void*)&stub_fopen}, {F_fclose, (void*)&stub_fclose}, {F_fread, (void*)&stub_fread}, {F_fwrite, (void*)&stub_fwrite},
        {F_ftell, (void*)&stub_ftell}, {F_fseek, (void*)&stub_fseek}, {F_fprintf, (void*)&stub_fprintf}, {F_printf, (void*)&stub_printf},
        {F_sprintf, (void*)&stub_sprintf}, {F_sscanf, (void*)&stub_sscanf}, {F_strchr, (void*)&stub_strchr},
        {F_stricmp, (void*)&stub_stricmp}, {F_FileOpen, (void*)&stub_FileOpen}, {F_FileClose, (void*)&stub_FileClose},
        {F_FileReadLine, (void*)&stub_FileReadLine}, {F_mrModelInfoGet, (void*)&stub_mrModelInfoGet},
        {F_mrModelInfoForget, (void*)&stub_mrModelInfoForget},
    };
    for (auto& st : stubs) patch_jmp(st.at, st.to);
    // the arena with 4 MB reserved and never committed after it (and before): a write past it (a damaged .3ds's counts) faults, the same
    // in both passes, rather than landing in this program's own memory
    // (and before it: a damaged .3ds's vertex count past 32767 turns negative as a short, and parse_object's fixup runs
    // from there)
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES + 0x800000, MEM_RESERVE, PAGE_NOACCESS) + 0x400000;
    VirtualAlloc(g_arena, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();

    // the models and the .3ds templates: each model written by the original Write3DS, then generated trees
    load_models(install);
    build_world();
    randomize_statics();
    HS->heap_next = A_HEAP;
    {
        std::vector<int> picks;
        for (int i = 0; i < g_stock_models && picks.size() < 6; i++) {
            const int32_t* c = (const int32_t*)g_models[i].data();
            if (c[0] <= 1500 && c[4] <= 1500) picks.push_back(i);
        }
        for (int k = 0; k < 10; k++) {
            const std::vector<uint8_t> m = k < (int)picks.size() ? g_models[picks[k]] : gen_model();
            MrModelInfo* info = place_model(m, W.model);
            memset(HS->files, 0, sizeof HS->files);
            memset(HS->streams, 0, sizeof HS->streams);
            for (int i = 0; i < NFILES; i++) strcpy(HS->files[i].name, k_file_names[i]);
            HS->write_fail_at = -1;
            HS->wopen_fail = 0;
            call_orig(0x004ba440, {U(info), U(W.names[4])});
            if (!g_setup_ret) { printf("setup: the original Write3DS failed\n"); continue; }
            File3DS f;
            f.d.assign(file_data(F_EXPORT), file_data(F_EXPORT) + HS->files[F_EXPORT].size);
            walk_chunks(f, 0, (uint32_t)f.d.size(), 0);
            g_3ds.push_back(f);
        }
        for (int k = 0; k < 30; k++) g_3ds.push_back(gen_3ds());
    }
    memset(g_arena, 0, ARENA_BYTES);
    build_world();
    randomize_statics();
    HS->heap_next = A_HEAP;
    mem_save(g_pristine);
    size_t chunks = 0;
    for (auto& f : g_3ds) chunks += f.chunks.size();
    printf("world: %d stock models from %s, %u .3ds files (%u chunks; %d written by the original Write3DS)\n", g_stock_models,
           install, (unsigned)g_3ds.size(), (unsigned)chunks, (int)g_3ds.size() - 30);

    int dup = 0, missing = 0;
    for (int i = 0; i < g_nfns; i++)
        for (int j = i + 1; j < g_nfns; j++)
            if (g_fns[i].v10 == g_fns[j].v10 || !strcmp(g_fns[i].name, g_fns[j].name)) {
                printf("  listed twice: %08x %s / %08x %s\n", g_fns[i].v10, g_fns[i].name, g_fns[j].v10, g_fns[j].name);
                dup++;
            }
    for (auto& e : g_inv) {
        bool found = false;
        for (int i = 0; i < g_nfns; i++) found |= g_fns[i].v10 == e.first;
        if (!found) { printf("  not rewritten: %08x %s\n", e.first, e.second.c_str()); missing++; }
    }
    for (int i = 0; i < g_nfns; i++) {
        bool found = false;
        for (auto& e : g_inv) found |= g_fns[i].v10 == e.first;
        if (!found) { printf("  not in the three objects: %08x %s\n", g_fns[i].v10, g_fns[i].name); missing++; }
    }

    static Footprint fp;
    long long checks = 0, poisoned_checks = 0, log_words = 0;
    int differ = 0, fp_bad = 0, faults = 0, pure_n = 0, replay_n = 0, changed_fns = 0, still_fns = 0, bad_fns = 0, skipped = 0;
    long long both_fault = 0, fp_checks = 0;
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only && !strstr(f.name, only)) continue;
        bool fn_bad = false, fn_changed = false;
        int fn_faults = 0, fn_checks = 0, fn_fpck = 0;
        long long fn_words = 0;
        const int nr = is_slow(f.v10) ? rounds * 3 : rounds * 10;
        for (int rd = 0; rd < nr && !fn_bad; rd++) {
            const bool poisoned = rd & 1;
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            if (poisoned) poison(g_pristine);
            randomize_statics();
            random_world();
            uint32_t words[72];
            if (!make_args(f, words)) { skipped++; continue; }
            if (step) printf("  %08x %s round %d\n", f.v10, f.name, rd);
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            mem_save(g_snap);
            g_cur_fn = f.name; g_cur_round = rd; g_cur_pass = 0;
            const Result ro = run(f, false, words);
            g_cur_pass = 2;
            const bool changed = mem_diff(g_snap) != 0;
            fn_changed |= changed;
            if (!fp.replay_only && !ro.fault) {
                fn_fpck++;
                fp_checks++;
                uint32_t out = 0;
                auto covered = [&](const uint8_t* p) {
                    for (int k = 0; k < fp.n; k++) if (p >= (uint8_t*)fp.r[k].p && p < (uint8_t*)fp.r[k].p + fp.r[k].n) return true;
                    return false;
                };
                for (uint32_t i2 = next_diff(g_snap.data, DATA, DATA_BYTES, 0); i2 < DATA_BYTES && !out; i2 = next_diff(g_snap.data, DATA, DATA_BYTES, i2 + 1))
                    if (fp.pure || !covered(DATA + i2)) out = 0x004e1000 + i2;
                for (uint32_t i2 = next_diff(g_snap.arena, g_arena, ARENA_BYTES, A_WORLD); i2 < ARENA_BYTES && !out;
                     i2 = next_diff(g_snap.arena, g_arena, ARENA_BYTES, i2 + 1))
                    if (!covered(g_arena + i2)) out = U(g_arena + i2);
                if (out) {
                    printf("  FOOTPRINT %08x %s: the original changed %08x (arena +%x) outside it (round %d)\n", f.v10, f.name, out,
                           out - U(g_arena), rd);
                    if (getenv("VP_DEBUG_WORLD"))
                        for (int k = 0; k < fp.n; k++) printf("    footprint %08x +%x %s\n", U(fp.r[k].p), fp.r[k].n, fp.r[k].what);
                    fp_bad++;
                    fn_bad = true;
                }
            }
            mem_save(g_after);
            memcpy(&g_log_orig, &g_log, sizeof(uint32_t) * (1 + (g_log.n < LOG_MAX ? g_log.n : LOG_MAX)));
            mem_load(g_snap);
            g_cur_pass = 1;
            const Result rn = run(f, true, words);
            g_cur_pass = 3;
            // the 16th byte of a triangle made from a copy on the stack is never set: whatever the stack held where the
            // original's frame lies (a caller's frame of another size moves it) -- never read; taken as the original's
            if (f.v10 == F_do_3dface || f.v10 == F_do_polyline || f.v10 == F_ModBuilderImportMI || f.v10 == 0x004b8fa0 ||
                f.v10 == 0x004b9800 || f.v10 == 0x004b9890)
                for (int b = 0; b < 2; b++)
                    for (int t = 0; t < W.mb[b]->maxtris; t++) {
                        const uint32_t at = U(&W.mb[b]->tris[t].pad) - U(g_arena);
                        g_arena[at] = g_after.arena[at];
                    }
            checks++;
            fn_checks++;
            poisoned_checks += poisoned;
            log_words += g_log_orig.n;
            fn_words += g_log_orig.n;
            if (ro.fault || rn.fault) { faults++; fn_faults++; }
            if (ro.fault && rn.fault) both_fault++;
            bool same = ro.fault == rn.fault && ro.code == rn.code && ro.ret == rn.ret && ro.pops == rn.pops && ro.top == rn.top &&
                        !memcmp(&ro.st0, &rn.st0, sizeof ro.st0) && !memcmp(ro.regs, rn.regs, sizeof ro.regs);
            const uint32_t where = mem_diff(g_after);
            same &= where == 0;
            same &= g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
            if (!same) {
                printf("  MISMATCH %08x %s (round %d, %s, pc %s)\n", f.v10, f.name, rd, poisoned ? "poisoned" : "as loaded", g_pc == _PC_24 ? "24" : "53");
                printf("    fault %d/%d (%08x at %08x / %08x at %08x), return %08x / %08x, x87 top %u / %u, popped %u / %u,"
                       " ebx esi edi ebp %08x %08x %08x %08x / %08x %08x %08x %08x\n",
                       ro.fault, rn.fault, ro.code, ro.eip, rn.code, rn.eip, ro.ret, rn.ret, ro.top, rn.top, ro.pops, rn.pops,
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
        if (trace)
            printf("%08x %-40s %s%s (%d checks, %d with the footprint checked, %d faulted, %lld words logged a check)\n", f.v10, f.name,
                   fn_bad ? "BAD" : "ok", fn_changed ? "" : " (never changed memory)", fn_checks, fn_fpck, fn_faults,
                   fn_checks ? fn_words / fn_checks : 0);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    printf("%d functions (%d pure, %d replay_only in round 0, %d others), %d listed twice, %d missing or extra; %lld checks (%lld on "
           "poisoned .data, %lld with the footprint checked), %lld logged words compared: %d differ, %d footprint violations, %d "
           "checks faulted (%lld in both, the same way), %d skipped; %d functions bad\n", g_nfns, pure_n, replay_n,
           g_nfns - pure_n - replay_n, dup, missing, checks, poisoned_checks, fp_checks, log_words, differ, fp_bad, faults, both_fault,
           skipped, bad_fns);
    printf("%d functions changed memory in some round, %d never did\n", changed_fns, still_fns);
    if (getenv("VP_COVER"))
        for (auto& c : g_cover)
            printf("  %8lld x %s\n", c.second, readable((const void*)(uintptr_t)c.first, 1) ? (const char*)(uintptr_t)c.first : "?");
    return differ || fp_bad || dup || missing ? 1 : 0;
}
