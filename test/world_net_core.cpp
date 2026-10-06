// world_net_core.cpp -- multiplayer stage N1, group B: every rewrite of hook/net_core.cpp (delqueue.obj, datamod.obj,
// dataport.obj, hostent.obj, nettypes.obj), hook/net_session.cpp (session.obj) and the generated hook/net_leftover.cpp
// (the $E initialisers of the whole multi library, the mechanical stubs and deleting destructors) against its original,
// outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_net_core.cpp
//        /Fo<dir>\ /Fe<dir>\world_net_core.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//        /STACK:0x800000,0x800000
//   run:   world_net_core.exe [rounds] [seed]     (VP_TRACE=1: one line per function; VP_ONLY=text: those only;
//          VP_DEBUG_WORLD=1: every fault's registers)
//     (and with /DVP_NET_FIXES: the fix build, below)
//
// Loads out\race_v10.exe at 0x400000 the way test/world_leftover.cpp does (a child process with the range reserved) and
// includes the three files with PORT_FN redefined to list each function (its v1.0 address, the rewrite, __cdecl or
// __thiscall-as-__fastcall, its stack arguments and return width, its footprint). VP_FAITHFUL.
//
// Once: every $E initialiser of the multi library (the original), so the class masks, the Xlators' keys and the tables are
// as the game has them. Then two phases.
//
// The leftovers (net_leftover.cpp, world_leftover.cpp's method): for each, `rounds` x 4 times, .data/.bss as set up (half
// the rounds poisoned: every byte random), random argument words (ecx a pointer into the arena), the original then the
// rewrite from one snapshot; the destructors the deleting destructors call are logging stubs for this phase.
//
// The hand-written functions: for each, `rounds` x 40 times, a world is BUILT in a 1 MB arena by the original code
// (CreateSessionMgr over a fake ISocket; its channels, local and remote services, password and async lookup made random;
// SessionData queued in both lists through the manager's own pool; hosts on its ReliableDataPort through the original
// get_entry, each with sent / waiting / held packets and the port's ready list, ids around the hosts' counters, callbacks
// 0, a logging stub or the session's own sess_rdp_cb; a DelayQueue, a DataModerator with queued records, a DataPort; car
// packets with random -- sometimes NaN, infinite, huge -- floats), a random harness state (the clock and its step, the
// random numbers, the task, how long the link stays alive, the packets the socket will hand out -- session control of
// every type, channel data, acks, reliable data in and out of order -- which allocations fail), and arguments made for
// the function from it. The stack the call uses is filled with one pattern first (the original's frame garbage -- byte 1
// of the session packets, the RPI tails -- is then the same in both passes). The ORIGINAL runs from a raw call thunk;
// its results kept, the snapshot restored, the REWRITE runs. Compared: .data/.bss/.idata and the arena (the world's
// objects and pools, the harness state), the return value, bytes popped, ebx / esi / edi / ebp, the x87 stack depth,
// faults, and every stub's call log. A function that isn't replay_only may change only its footprint (checked on the
// original's pass; the harness state -- clock, random numbers, allocator -- excepted). The x87 at 24 or 53 bits.
//
// Stubbed (logged; their state in the arena): MemAlloc (a bump allocator; some calls fail), MemFree, operator delete,
// LogReport / LogPanic (the format and its arguments, strings by content), PTimeNow (steps), Random, TaskGetID /
// TaskGetName, Xlator::xlate (the text := the key), atexit, sprintf, stricmp, and the fake ISocket's slots (Send by the
// packet's bytes and address, Recv from the scripted queue, LinkAlive's countdown, the addresses, MakeStr, the host
// lookups). The game's own code everywhere else: PoolBase, Xlator's constructor, memmove / strncpy, CIacos, the PTime
// conversions, MultiGetPacketDelay, operator== / != (socket_addr), and every function of this group (each rewrite is
// checked against its original with the original callees). find_range and OfferUnboundService get worlds whose free-
// looking channels have no flags (the original loops forever on one with flags and no service: fixed, see below).
//
// Built with /DVP_NET_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Multiplayer").
// Every function is still compared as above; a round where a fix changed what the rewrite did (the fixes mark it:
// VP_FIX_HIT) may differ from the original, and is counted (per fix, listed) instead -- the rewrite must still return
// cleanly there. Then directed_fix_tests: for each fix, the bad case on the original (its fault, overrun, hang -- run on
// a thread with a time limit -- or panic shown) and on the rewrite (no fault, nothing written outside what it may write,
// the result the fix promises), and the boundary case that still fits on both, compared bit for bit. Without it
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

#ifndef VP_NET_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#define NET_FIXES 0
#else
#define NET_FIXES 1
#endif
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif
#include "../hook/port.h"

// ---- the registry -----------------------------------------------------------------------------------------------------------
struct Ent {
    uint32_t v10;
    const char* name;
    void* fn;
    int fast, nstack, ret;
    void (*fp)(Footprint&, const uint32_t*);
    bool leftover;
};
static Ent g_fns[2048];
static int g_nfns;
static bool g_reg_leftover;
struct EntReg { EntReg(Ent e) { e.leftover = g_reg_leftover; if (g_nfns < 2048) g_fns[g_nfns++] = e; } };
template <typename T> static T from_word(uint32_t w) { T t; memcpy(&t, &w, sizeof(T)); return t; }
template <typename R> static constexpr int ret_width() { if constexpr (std::is_void_v<R>) return 0; else return (int)sizeof(R); }
template <typename F> struct Sig;
template <typename R, typename... A> struct Sig<R(__cdecl*)(A...)> {
    enum { fast = 0, nstack = sizeof...(A) };
    static constexpr int ret = ret_width<R>();
    template <void (*FP)(Footprint&, A...), size_t... I> static void fpi(Footprint& f, const uint32_t* w, std::index_sequence<I...>) {
        FP(f, from_word<A>(w[I])...);
        (void)w;
    }
    template <void (*FP)(Footprint&, A...)> static void fp(Footprint& f, const uint32_t* w) { fpi<FP>(f, w, std::index_sequence_for<A...>()); }
};
template <typename R, typename... A> struct Sig<R(__fastcall*)(A...)> {
    enum { fast = 1, nstack = sizeof...(A) - 2 };
    static constexpr int ret = ret_width<R>();
    template <void (*FP)(Footprint&, A...), size_t... I> static void fpi(Footprint& f, const uint32_t* w, std::index_sequence<I...>) {
        FP(f, from_word<A>(w[I])...);
    }
    template <void (*FP)(Footprint&, A...)> static void fp(Footprint& f, const uint32_t* w) { fpi<FP>(f, w, std::index_sequence_for<A...>()); }
};
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                              \
    static EntReg VP_CAT(reg_, NEW)({V10, NAME, (void*)&NEW, Sig<decltype(&NEW)>::fast, Sig<decltype(&NEW)>::nstack, \
                                     Sig<decltype(&NEW)>::ret, &Sig<decltype(&NEW)>::fp<&FP>});

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { add(obj, 4, what); }
void Footprint::stack_ptr(void* p, const char* what) { add(p, 4, what); }

// what net_wsock.h gives the rewrites: here, N0's sites unpatched (the plain calls)
int __cdecl net_PTimeNow() { return ((int(__cdecl*)())(uintptr_t)0x00413b40)(); }
int __cdecl net_Random(int r) { return ((int(__cdecl*)(int))(uintptr_t)0x0041b6e0)(r); }

// the fixes mark where they changed what happens (net_core.cpp / net_session.cpp: nothing in the DLL)
static int g_fix_hits;
static const char* g_fix_what;
#define VP_FIX_HIT(what) (g_fix_hits++, (void)(g_fix_what = (what)))

#include "../hook/net_core.cpp"
#include "../hook/net_session.cpp"
static bool g_mark = (g_reg_leftover = true);
#include "../hook/net_leftover.cpp"

using namespace netc;
static uint8_t hg8(uint32_t a) { return *(volatile uint8_t*)(uintptr_t)a; }

// ---- random numbers -----------------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static int irange(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float frange(float a, float b) { return a + (b - a) * uni(); }
static float wildf(float lo, float hi) {
    switch (rnd() % 12) {
    case 0: return bitsf(0x7fc00000u | (rnd() & 0x3fffff));
    case 1: return (rnd() & 1) ? INFINITY : -INFINITY;
    case 2: return 0.0f;
    case 3: return -0.0f;
    case 4: return bitsf(rnd() & 0x807fffff);
    case 5: return frange(-1e30f, 1e30f);
    case 6: return bitsf(rnd());
    default: return frange(lo, hi);
    }
}

// ---- the original, loaded at 0x400000 -----------------------------------------------------------------------------------------
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
struct Patch { uint32_t at; uint8_t old[5]; };
static Patch g_patches[64];
static int g_npatches;
static void patch_jmp(uint32_t at, void* to, bool remember = false) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    if (remember && g_npatches < 64) { g_patches[g_npatches].at = at; memcpy(g_patches[g_npatches].old, p, 5); g_npatches++; }
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}
static void unpatch_remembered() {
    for (int i = 0; i < g_npatches; i++) memcpy((void*)(uintptr_t)g_patches[i].at, g_patches[i].old, 5);
    g_npatches = 0;
}

// ---- memory ------------------------------------------------------------------------------------------------------------------
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
static uint32_t mem_diff(const Mem& m) {
    if (memcmp(m.data, DATA, DATA_BYTES))
        for (uint32_t i = 0; i < DATA_BYTES; i++) if (m.data[i] != DATA[i]) return 0x004e1000 + i;
    if (memcmp(m.idata, IDATA, IDATA_BYTES))
        for (uint32_t i = 0; i < IDATA_BYTES; i++) if (m.idata[i] != IDATA[i]) return 0x005d7000 + i;
    if (memcmp(m.arena, g_arena, ARENA_BYTES))
        for (uint32_t i = 0; i < ARENA_BYTES; i++) if (m.arena[i] != g_arena[i]) return (uint32_t)(uintptr_t)(g_arena + i);
    return 0;
}

// ---- the harness state, in the arena (restored with it: both passes see the same) ------------------------------------------------
enum : uint32_t { A_STATE = 0x0, A_SOCK = 0x100, A_ASYNC = 0x200, A_ADDRS = 0x900, A_RECVQ = 0x1000, A_SCRATCH = 0x4000,
                  A_HEAP = 0x10000 };
struct HState {
    uint32_t brk;            // the bump allocator's next free byte (an arena offset)
    uint32_t alloc_calls;
    uint32_t fail_mask;      // MemAlloc call k (k < 32) fails when bit k is set
    int32_t now, step;       // PTimeNow: now += step each call
    uint32_t rnd;            // Random
    int32_t task_cur;        // TaskGetID
    int32_t alive;           // LinkAlive answers 1 this many more times
    uint32_t host_ok, async_ok, local_ok;   // GetHostAddr / AsyncGetHostAddr / AddressIsLocal answers
    uint32_t recv_n, recv_i; // the scripted packets
    uint32_t cookie_pad;
};
static HState* HS() { return (HState*)(g_arena + A_STATE); }
struct RecvPkt { int32_t len; uint8_t addr[12]; uint8_t data[0xf0]; };
static_assert(sizeof(RecvPkt) <= 0x100, "RecvPkt");
static RecvPkt* recvq(uint32_t i) { return (RecvPkt*)(g_arena + A_RECVQ + 0x100 * i); }
static uint8_t* addr_k(uint32_t k) { return g_arena + A_ADDRS + 12 * (k & 3); }

// ---- the stubs' call log -------------------------------------------------------------------------------------------------------
enum { LOG_MAX = 8192 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static bool g_logging = true;
static void L(uint32_t w) { if (!g_logging) return; if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static uint32_t fnv(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static void L_bytes(const void* p, uint32_t n) {
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] { L(n); __asm__ volatile("" ::: "memory"); L(fnv(p, n)); })) { L('BADP'); }
#else
    __try { L(n); L(fnv(p, n)); } __except (EXCEPTION_EXECUTE_HANDLER) { L('BADP'); }
#endif
}
static void L_str(const char* s) {
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] { const size_t n = strnlen(s, 4096); L((uint32_t)n); __asm__ volatile("" ::: "memory"); L(fnv(s, n)); })) { L('BADS'); }
#else
    __try { const size_t n = strnlen(s, 4096); L((uint32_t)n); L(fnv(s, n)); } __except (EXCEPTION_EXECUTE_HANDLER) { L('BADS'); }
#endif
}
// a pointer argument: its address if it's in the arena or the game (the same in both passes), else what it points at
static void L_ptr(uint32_t w, uint32_t n) {
    const uint32_t a = (uint32_t)(uintptr_t)g_arena;
    if ((w >= a && w < a + ARENA_BYTES) || (w >= 0x400000 && w < 0x700000) || w < 0x10000) L(w);
    else L_bytes((const void*)(uintptr_t)w, n);
}

// a game format and its arguments: %s by content, the rest by value
static void L_fmt(const char* fmt, const uint32_t* a) {
    L((uint32_t)(uintptr_t)fmt);
    int k = 0;
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] {                                        // (the barrier: the log words so far in memory before *p)
        for (const char* p = fmt; ({ __asm__ volatile("" ::: "memory"); *p; }) && k < 8; p++) {
            if (*p != '%') continue;
            p++;
            if (*p == '%') continue;
            while (*p && strchr("-+ #0123456789.lh", *p)) p++;
            if (!*p) break;
            if (*p == 's') L_str((const char*)(uintptr_t)a[k]);
            else L(a[k]);
            k++;
        }
    })) { L('BADF'); }
#else
    __try {
        for (const char* p = fmt; *p && k < 8; p++) {
            if (*p != '%') continue;
            p++;
            if (*p == '%') continue;
            while (*p && strchr("-+ #0123456789.lh", *p)) p++;
            if (!*p) break;
            if (*p == 's') L_str((const char*)(uintptr_t)a[k]);
            else L(a[k]);
            k++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { L('BADF'); }
#endif
}
static void __cdecl stub_LogReport(const char* fmt, ...) { L('LREP'); L_fmt(fmt, (const uint32_t*)(&fmt + 1)); }
static void __cdecl stub_LogPanic(const char* fmt, ...) { L('LPAN'); L_fmt(fmt, (const uint32_t*)(&fmt + 1)); }
static void* __cdecl stub_MemAlloc(int n) {
    HState* s = HS();
    const uint32_t k = s->alloc_calls++;
    L('MALC'); L((uint32_t)n);
    if (n < 0 || n > 0x40000 || (k < 32 && (s->fail_mask >> k & 1))) return 0;
    const uint32_t at = (s->brk + 15) & ~15u;
    if (at + (uint32_t)n > ARENA_BYTES - 0x100) return 0;
    s->brk = at + (uint32_t)n;
    return g_arena + at;
}
static void __cdecl stub_MemFree(void* p) { L('MFRE'); L((uint32_t)(uintptr_t)p); }
static void __cdecl stub_delete(void* p) { L('DEL '); L((uint32_t)(uintptr_t)p); }
static int __cdecl stub_PTimeNow() { HState* s = HS(); s->now += s->step; L('PTIM'); return s->now; }
static int __cdecl stub_Random(int range) {
    HState* s = HS();
    s->rnd = s->rnd * 1103515245u + 12345u;
    L('RAND'); L((uint32_t)range);
    return range > 0 ? (int)((s->rnd >> 8) % (uint32_t)range) : 0;
}
static int __cdecl stub_TaskGetID() { L('TKID'); return HS()->task_cur; }
static char g_task_names[4][16] = {"main", "lobby", "physics", "other"};
static char* __cdecl stub_TaskGetName(int id) { L('TKNM'); L((uint32_t)id); return g_task_names[(uint32_t)id & 3]; }
static void __fastcall stub_xlate(uint32_t* x, int) {
    L('XLAT'); L((uint32_t)(uintptr_t)x);
    x[1] = x[0];
    x[2] = *(uint32_t*)(uintptr_t)G_XLATOR_COOKIE;
}
static int __cdecl stub_atexit(void* f) { L('ATEX'); L((uint32_t)(uintptr_t)f); return 0; }
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    L('SPRF'); L((uint32_t)(uintptr_t)fmt); L_str(buf);
    return r;
}
static int __cdecl stub_stricmp(const char* a, const char* b) { L('STRI'); L_str(a); L_str(b); return _stricmp(a, b); }
// the game's FOOD and ASSERT_MSG are bare `ret`s; in the hand-written phase they log, so the branches show in the logs
static void __cdecl stub_FOOD(const char* fmt, ...) { L('FOOD'); L_fmt(fmt, (const uint32_t*)(&fmt + 1)); }
static void __cdecl stub_ASSERT(int cond, const char* fmt, ...) {
    const uint32_t* a = (const uint32_t*)(&fmt + 1);
    L('ASRT'); L((uint32_t)cond); L((uint32_t)(uintptr_t)fmt); L(a[0]); L(a[1]); L(a[2]);                 // (hostent.obj's has 3 after fmt, session.obj's 4)
}
static void __cdecl stub_rpi_cb(uint8_t res, uint32_t* data) { L('RPCB'); L(res); L(data[0]); L(data[1]); }
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }
// the deleting destructors' destructors (the leftovers phase only)
template <uint32_t A> static void __fastcall stub_dtor(void* self, int) { L('DTOR'); L(A); L((uint32_t)(uintptr_t)self); }

// ---- the fake ISocket ------------------------------------------------------------------------------------------------------------
static void* __fastcall so_dtor(void* self, int, uint32_t fl) { L('SDTR'); L(fl); return self; }
// the bytes of a session packet its builder never writes (net_wsock.cpp's net_send_mask, for what this group builds): byte 1
// of an unreliable session-control packet, a ConnectRequest's password after its NUL
static void mask_session_packet(uint8_t* p, int n) {
    if (n < 4 || (p[0] & 0xf) != 0 || p[2] != 0x1e) return;
    if ((p[0] >> 4) == 7) p[1] = 0;
    if (p[3] == 0)
        for (int i = 9; i < 25 && i < n; i++)
            if (!p[i]) { for (int j = i + 1; j < 25 && j < n; j++) p[j] = 0; break; }
}
static void __fastcall so_Send(void*, int, const uint8_t* pkt, int len, const uint8_t* addr) {
    L('SEND'); L((uint32_t)len);
    uint8_t m[0x400];
    const int n = len > 0 && len < 0x400 ? len : 0;
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] { memcpy(m, pkt, n); })) { L('BADP'); return; }
#else
    __try { memcpy(m, pkt, n); } __except (EXCEPTION_EXECUTE_HANDLER) { L('BADP'); return; }
#endif
    mask_session_packet(m, n);
    L_bytes(m, (uint32_t)n);
    L_bytes(addr, 12);
}
static uint8_t __fastcall so_Recv(void*, int, uint8_t* buf, int* len, uint8_t* addr) {
    HState* s = HS();
    L('RECV'); L((uint32_t)*len);
    if (s->recv_i >= s->recv_n) return 0;
    RecvPkt* p = recvq(s->recv_i++);
    const int n = p->len < *len ? p->len : *len;
    memcpy(buf, p->data, n > 0 ? n : 0);
    *len = n;
    if (addr) memcpy(addr, p->addr, 12);
    return 1;
}
static uint8_t __fastcall so_AsyncGetHostAddr(void*, int, const char* name, uint8_t* blk) {
    L('SAGH'); L_str(name); L((uint32_t)(uintptr_t)blk);
    return (uint8_t)(HS()->async_ok & 1);
}
static void __fastcall so_AsyncCancel(void*, int, uint8_t* blk) { L('SACN'); L((uint32_t)(uintptr_t)blk); }
static uint8_t __fastcall so_GetHostAddr(void*, int, const char* name, uint8_t* addr) {
    L('SGHA'); L_str(name);
    memcpy(addr, addr_k(1), 12);
    return (uint8_t)(HS()->host_ok & 1);
}
static void __fastcall so_MakeStr(void*, int, const uint8_t* addr, char* buf) {
    L('SMKS'); L_bytes(addr, 12);
    sprintf(buf, "%02x%02x%02x%02x:%02x%02x", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}
static uint8_t __fastcall so_AddressIsLocal(void*, int, const uint8_t* addr) { L('SLOC'); L_bytes(addr, 12); return (uint8_t)(HS()->local_ok & 1); }
static void __fastcall so_GetMyAddr(void*, int, uint8_t* addr) { L('SMYA'); memcpy(addr, addr_k(0), 12); }
static void __fastcall so_GetBroadcastAddr(void*, int, uint8_t* addr) { L('SBCA'); memset(addr, 0xff, 12); addr[0] = 2; addr[1] = 0; }
static uint8_t __fastcall so_LinkAlive(void*, int) {
    HState* s = HS();
    L('SLNK');
    if (s->alive > 0) { s->alive--; return 1; }
    return 0;
}
static void* g_sock_vt[16] = {(void*)so_dtor, (void*)so_Send, (void*)so_Recv, (void*)so_AsyncGetHostAddr, (void*)so_AsyncCancel,
                              (void*)so_GetHostAddr, (void*)so_MakeStr, (void*)so_AddressIsLocal, (void*)so_GetMyAddr,
                              (void*)so_GetBroadcastAddr, (void*)so_LinkAlive, 0, 0, 0, 0, 0};

// ---- the raw call -------------------------------------------------------------------------------------------------------------
static uint32_t g_c_fn, g_c_ecx, g_c_edx, g_c_n, g_c_args[16];
static uint32_t g_r_eax, g_r_edx, g_r_ebx, g_r_esi, g_r_edi, g_r_ebp, g_r_esp, g_c_top, g_save_esp;
static uint16_t g_r_sw;
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((naked)) static void raw_call() {
    __asm__ volatile(
        "push ebx\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "push ebp\n\t"
        "mov dword ptr [%P[save_esp]], esp\n\t"
        "mov ecx, dword ptr [%P[c_n]]\n"
        "1:\n\t"                                          // pushloop
        "test ecx, ecx\n\t"
        "jz 2f\n\t"
        "dec ecx\n\t"
        "push dword ptr [%P[c_args] + ecx * 4]\n\t"
        "jmp 1b\n"
        "2:\n\t"                                          // pushed
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
        "mov esp, dword ptr [%P[save_esp]]\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "ret"
        :: [save_esp] "i"(&g_save_esp), [c_n] "i"(&g_c_n), [c_args] "i"(g_c_args), [c_top] "i"(&g_c_top),
           [c_ecx] "i"(&g_c_ecx), [c_edx] "i"(&g_c_edx), [c_fn] "i"(&g_c_fn), [r_eax] "i"(&g_r_eax), [r_edx] "i"(&g_r_edx),
           [r_ebx] "i"(&g_r_ebx), [r_esi] "i"(&g_r_esi), [r_edi] "i"(&g_r_edi), [r_ebp] "i"(&g_r_ebp), [r_esp] "i"(&g_r_esp),
           [r_sw] "i"(&g_r_sw));
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
        mov esp, g_save_esp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}
#endif
// the stack the call will use, filled with one pattern (world_root_race.cpp's): frame garbage the same in both passes
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((naked)) static void fill_stack() {
    __asm__ volatile(
        "push edi\n\t"
        "push ecx\n\t"
        "push eax\n\t"
        "lea edi, [esp - 4]\n\t"
        "mov ecx, 0xc000\n\t"
        "mov eax, 0xa5a5a5a5\n\t"
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
        mov eax, 0xa5a5a5a5
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
struct Result { int fault; uint32_t code, eip, ret, pops, regs[4], top; };
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
static unsigned g_pc = _PC_53;
__declspec(noinline) static Result run(const Ent& f, bool rewrite, const uint32_t* words) {
    Result r = {};
    g_log.n = 0;
    g_c_fn = rewrite ? (uint32_t)(uintptr_t)f.fn : f.v10;
    const uint32_t* stack = words;
    if (f.fast) { g_c_ecx = words[0]; g_c_edx = words[1]; stack = words + 2; }
    else { g_c_ecx = 0x00c0ffee; g_c_edx = 0x00d00d1e; }
    g_c_n = (uint32_t)f.nstack;
    for (int i = 0; i < f.nstack && i < 16; i++) g_c_args[i] = stack[i];
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
    }
    return r;
}
static uint32_t call_orig(uint32_t fn, int fast, std::initializer_list<uint32_t> words) {
    Ent e = {fn, "(setup)", 0, fast, (int)words.size() - (fast ? 2 : 0), 4, 0, false};
    uint32_t w[16];
    int i = 0;
    for (uint32_t x : words) w[i++] = x;
    const unsigned pc = g_pc;
    g_pc = _PC_53;
    const Result r = run(e, false, w);
    g_pc = pc;
    if (r.fault) printf("setup: %08x faulted (%08x at %08x)\n", fn, r.code, r.eip);
    return r.ret;
}
#define U(p) ((uint32_t)(uintptr_t)(p))

// ---- the world -------------------------------------------------------------------------------------------------------------------
struct World {
    uint8_t* sock;
    SessionMgr* sm;
    ReliableDataPort* rdp;
    HostEntry* hosts[8];
    int nhosts;
    DelayQueue* dq;
    DataModerator* dm;
    DataPort* dp;
    uint8_t* blk;
    uint8_t* scratch;          // arguments' buffers (0x6000 bytes from A_SCRATCH)
    uint32_t local_ids[8];
    int nlocal_ids;
};
static World W;
static uint32_t g_rpp_pool_off, g_dq_pool_off;          // arena offsets of the world's two pools (0: none)
static uint32_t g_scratch_at;
static uint8_t* sv(uint32_t n) {                  // a scratch buffer, random-filled
    g_scratch_at = (g_scratch_at + 15) & ~15u;
    uint8_t* p = g_arena + A_SCRATCH + g_scratch_at;
    g_scratch_at += n;
    if (g_scratch_at > 0xb000) { printf("scratch overflow\n"); ExitProcess(4); }
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)rnd();
    return p;
}

static const uint8_t k_flags[] = {0, 0, 0, 1, 1, 0x81, 0x81, 0x10, 0x84, 0x82, 8, 2, 0x80, 0x88, 0x18, 0x11, 9, 0x83};
static RPP* rpp_alloc() { return (RPP*)(uintptr_t)call_orig(F_PoolBase_alloc, 1, {U(&W.rdp->rpp_pool), 0}); }
static void random_rpi(RPI* r) {
    switch (rnd() % 4) {
    case 0: r->fn = 0; break;
    case 1: case 2: r->fn = (void*)stub_rpi_cb; break;
    default: r->fn = (void*)(uintptr_t)F_sess_rdp_cb; break;
    }
    void* m = W.sm;
    memcpy(r->data, &m, 4);
    r->data[4] = (uint8_t)(rnd() % 8);
    r->data[5] = (uint8_t)(rnd() % (W.sm->nsessions + 1));
    r->data[6] = (uint8_t)rnd();
    r->data[7] = (uint8_t)rnd();
}
static RPP* make_rpp(uint8_t id) {
    RPP* r = rpp_alloc();
    if (!r) return 0;
    r->len = irange(2, 0x40);
    for (int i = 0; i < r->len; i++) r->pkt[i] = (uint8_t)rnd();
    r->pkt[0] = (uint8_t)((rnd() % 8) << 4 | (rnd() & 0xf));
    r->pkt[1] = id;
    r->next = 0;
    random_rpi(&r->rpi);
    r->sent = HS()->now - irange(0, 3000);
    r->first_sent = r->sent - irange(0, 500);
    r->sends = irange(1, 7);
    return r;
}
static void chain(RPP** head, RPP* r) {
    if (!r) return;
    while (*head) head = &(*head)->next;
    *head = r;
}

// a session-layer packet for the socket to hand out (or for a recv_* call)
static int make_packet(uint8_t* p, int cap) {
    for (int i = 0; i < cap; i++) p[i] = (uint8_t)rnd();
    SessionMgr* m = W.sm;
    const int ns = m->nsessions;
    const uint32_t id = W.nlocal_ids && chance(70) ? W.local_ids[rnd() % W.nlocal_ids] : (uint32_t)irange(990, 1010);
    switch (rnd() % 10) {
    case 0: case 1: case 2: {                                   // session control
        p[0] = (uint8_t)(chance(80) ? 0x70 : (rnd() % 8) << 4);
        p[2] = chance(90) ? 0x1e : (uint8_t)rnd();
        p[3] = (uint8_t)(chance(90) ? rnd() % 8 : rnd() % 12);
        const uint8_t t = p[3];
        if (t == 0) { memcpy(p + 4, &id, 4); p[8] = (uint8_t)irange(0, ns); strcpy((char*)p + 9, chance(50) ? "" : chance(50) ? "pw" : "PW"); return 0x19; }
        if (t == 1 || t == 3) { p[4] = (uint8_t)irange(0, ns + 1); return 9; }
        if (t == 2) { p[4] = (uint8_t)rnd(); p[5] = (uint8_t)irange(0, ns + 1); return 6; }
        if (t == 4) { p[4] = (uint8_t)rnd(); if (chance(50) && ns) p[4] = m->sessions[rnd() % ns].remote; return 5; }
        if (t == 5) { const uint32_t ty = chance(70) ? 0x1001 : 0x1002; memcpy(p + 4, &ty, 4); return 8; }
        if (t == 6) {
            const uint32_t ty = chance(60) ? m->request_type : 0x1002;
            memcpy(p + 0x24, &ty, 4);
            const uint32_t rid = chance(50) && m->nremote ? m->remote[rnd() % m->nremote].id : (uint32_t)irange(990, 1010);
            memcpy(p + 0x2c, &rid, 4);
            return 0x44;
        }
        if (t == 7) {
            const uint32_t rid = chance(60) && m->nremote ? m->remote[rnd() % m->nremote].id : (uint32_t)irange(990, 1010);
            memcpy(p + 4, &rid, 4);
            return 8;
        }
        return irange(4, 0x40);
    }
    case 3: case 4: case 5:                                     // channel data
        p[0] = (uint8_t)(((rnd() % 8) << 4) | (chance(80) ? 1 + rnd() % 15 : 0));
        p[1] = (uint8_t)irange(0, ns);
        p[2] = (uint8_t)irange(0, ns);
        return irange(4, 0xe2);
    case 6:                                                     // an ack
        p[0] = (uint8_t)((rnd() % 2) << 4);
        p[1] = (uint8_t)rnd();
        return 2;
    default: {                                                  // reliable data
        p[0] = (uint8_t)(((2 + rnd() % 4) << 4) | (rnd() & 0xf));
        const HostEntry* h = W.nhosts ? W.hosts[rnd() % W.nhosts] : 0;
        p[1] = h ? (uint8_t)(h->recv_id + irange(-3, 5)) : (uint8_t)rnd();
        return irange(3, 0x60);
    }
    }
}

static void build_world(bool calm_channels) {
    HState* s = HS();
    memset(s, 0, sizeof *s);
    s->brk = A_HEAP;
    s->now = (int32_t)(rnd() % 100000) + 5000;
    s->step = chance(20) ? 0 : irange(1, 600);
    s->rnd = rnd();
    s->task_cur = (int32_t)(rnd() & 3);
    s->alive = 0;
    g_scratch_at = 0;
    memset(&W, 0, sizeof W);
    for (int k = 0; k < 4; k++) {
        uint8_t* a = addr_k(k);
        for (int i = 0; i < 12; i++) a[i] = (uint8_t)rnd();
        a[0] = 2; a[1] = 0;
    }
    W.sock = g_arena + A_SOCK;
    *(void***)W.sock = g_sock_vt;
    // the session manager
    const int ns = chance(5) ? 0 : irange(1, 8), nr = irange(0, 5), nl = irange(0, 4);
    W.sm = (SessionMgr*)(uintptr_t)call_orig(0x004a7670, 0, {U(W.sock), (uint32_t)ns, (uint32_t)nr, (uint32_t)nl});
    if (!W.sm) { printf("setup: CreateSessionMgr failed\n"); ExitProcess(4); }
    SessionMgr* m = W.sm;
    W.rdp = &m->rdp;
    if (chance(85)) m->task = s->task_cur;
    // local services
    m->nlocal = nl ? irange(0, nl) : 0;
    for (int i = 0; i < m->nlocal; i++) {
        LocalService* l = &m->local[i];
        for (int k = 0; k < 0x48; k++) ((uint8_t*)l)[k] = (uint8_t)rnd();
        l->type = chance(70) ? 0x1001 : 0x1002;
        l->id = 1000 + (uint32_t)irange(0, 6);
        l->has_password = chance(40) ? (uint8_t)irange(1, 255) : 0;
        int lo = ns ? irange(0, ns - 1) : 0, hi = ns ? irange(lo, ns) : 0;
        l->lo = (int16_t)lo; l->hi = (int16_t)hi;
        strcpy(l->password, chance(50) ? "pw" : chance(50) ? "PW" : "other");
        l->refuse = chance(25) ? 1 : 0;
        W.local_ids[W.nlocal_ids++] = l->id;
    }
    // channels
    for (int i = 0; i < ns; i++) {
        SessionInfo* c = &m->sessions[i];
        c->flags = calm_channels ? 0 : k_flags[rnd() % sizeof k_flags];
        c->service = W.nlocal_ids && chance(50) ? W.local_ids[rnd() % W.nlocal_ids] : 0;
        c->remote = (uint8_t)irange(0, 9);
        memcpy(&c->addr, addr_k(rnd()), 12);
        c->reason = (int32_t)irange(0, 8);
        c->queue = 0;
    }
    // remote services
    m->nremote = nr ? irange(0, nr) : 0;
    for (int i = 0; i < m->nremote; i++) {
        RemoteService* r = &m->remote[i];
        for (int k = 0; k < 0x54; k++) ((uint8_t*)r)[k] = (uint8_t)rnd();
        r->id = chance(60) ? 1000 + (uint32_t)irange(0, 6) : rnd();
        memcpy(&r->addr, addr_k(rnd()), 12);
        r->fresh = (uint8_t)(rnd() & 1);
    }
    m->request_time = chance(30) ? 0 : s->now - irange(0, 8000);
    m->request_type = chance(20) ? 0 : chance(70) ? 0x1001 : 0x1002;
    m->status_at = ns ? irange(-1, ns - 1) : -1;
    strcpy(m->password, chance(50) ? "" : chance(50) ? "pw" : "Pw");
    // the async lookup
    W.blk = g_arena + A_ASYNC;
    for (int k = 0; k < 0x600; k++) W.blk[k] = (uint8_t)rnd();
    *(int32_t*)(W.blk + ASRB_STATE) = irange(0, 3);
    W.blk[ASRB_MSG] = 0;
    m->async = chance(50) ? W.blk : 0;
    m->async_port = (int16_t)rnd();
    // received channel data, in both lists
    const int nd = ns ? irange(0, 6) : 0;
    for (int k = 0; k < nd; k++) {
        SessionData* e = (SessionData*)(uintptr_t)call_orig(F_PoolBase_alloc, 1, {U(&m->data_pool), 0});
        if (!e) break;
        e->next = 0; e->snext = 0;
        e->sess = irange(0, ns - 1);
        e->len = irange(1, 0xe4);
        for (int i = 0; i < 0xe4; i++) e->data[i] = (uint8_t)rnd();
        SessionData** q = &m->sessions[e->sess].queue;
        while (*q) q = &(*q)->snext;
        *q = e;
        SessionData** g = &m->data;
        while (*g) g = &(*g)->next;
        *g = e;
    }
    // hosts on the reliable port, with their packets
    W.nhosts = irange(0, 4);
    for (int k = 0; k < W.nhosts; k++) {
        HostEntry* h = (HostEntry*)(uintptr_t)call_orig(F_RDP_get_entry, 1, {U(W.rdp), 0, U(addr_k(k)), 1});
        if (!h) { W.nhosts = k; break; }
        W.hosts[k] = h;
        h->rtt_n = chance(20) ? irange(1, 2) : chance(10) ? irange(0x3fe, 0x402) : irange(1, 40);
        h->rtt_sum = irange(0, 200000);
        h->send_id = (uint8_t)rnd();
        h->send_first = (uint8_t)(chance(30) ? 1 : 0);
        h->recv_id = (uint8_t)rnd();
        h->recv_first = (uint8_t)(chance(20) ? 1 : 0);
        const int nsent = irange(0, 3), nwait = irange(0, 2), nheld = irange(0, 3);
        for (int i = 0; i < nsent; i++) chain(&h->sent, make_rpp((uint8_t)(h->send_id - (nsent - 1 - i) - (chance(20) ? 2 : 0))));
        for (int i = 0; i < nwait; i++) chain(&h->waiting, make_rpp((uint8_t)rnd()));
        uint8_t id = (uint8_t)(h->recv_id + irange(1, 3));
        for (int i = 0; i < nheld; i++) { chain(&h->held, make_rpp(id)); id = (uint8_t)(id + irange(1, 3)); }
    }
    const int nready = irange(0, 2);
    for (int i = 0; i < nready; i++) {
        RPP* r = make_rpp((uint8_t)rnd());
        if (r) memcpy(&r->rpi, addr_k(rnd()), 12);
        chain(&W.rdp->ready, r);
    }
    // a DelayQueue, a DataModerator with records, a DataPort
    W.dq = (DelayQueue*)(uintptr_t)call_orig(F_MemAlloc, 0, {0x10});
    call_orig(F_DelayQueue_ctor, 1, {U(W.dq), 0, (uint32_t)irange(1, 6), (uint32_t)irange(8, 64)});
    const int nq = irange(0, 4);
    for (int i = 0; i < nq; i++) {
        uint8_t* d = sv(64);
        call_orig(F_DelayQueue_Enqueue, 1, {U(W.dq), 0, U(d), (uint32_t)irange(0, W.dq->size), rnd()});
    }
    W.dm = (DataModerator*)(uintptr_t)call_orig(F_MemAlloc, 0, {0x14});
    call_orig(0x004afc70, 1, {U(W.dm), 0, U(W.sock)});
    const int nm = irange(0, 3);
    for (int i = 0; i < nm; i++) {
        uint8_t* d = sv(0xe4);
        call_orig(0x004afcc0, 1, {U(W.dm), 0, U(d), (uint32_t)irange(1, 0xe4), U(addr_k(rnd()))});
    }
    W.dp = (DataPort*)(uintptr_t)call_orig(F_MemAlloc, 0, {0xc});
    call_orig(F_DataPort_ctor, 1, {U(W.dp), 0, U(W.sock)});
    if (chance(50)) W.dp->mod = W.dm;
    // the socket's script
    s->recv_n = (uint32_t)irange(0, 6);
    for (uint32_t i = 0; i < s->recv_n; i++) {
        RecvPkt* p = recvq(i);
        p->len = make_packet(p->data, 0xf0);
        memcpy(p->addr, addr_k(rnd()), 12);
    }
    s->alive = chance(15) ? 0 : irange(1, 8);
    s->host_ok = rnd(); s->async_ok = rnd(); s->local_ok = rnd();
    // the clock moves on; some of the test's allocations fail
    s->now += irange(0, 4000);
    s->alloc_calls = 0;
    s->fail_mask = chance(20) ? rnd() & rnd() : 0;
    // the statics the code reads
    *(uint32_t*)(uintptr_t)G_SERVICE_ID = chance(30) ? 0 : rnd() % 100000;
    for (int i = 0; i < 8; i++) *(uint8_t*)(uintptr_t)(G_HOST_SLOTS + i) = (uint8_t)(chance(60) ? 1 : 0);
    // the function-local Xlators: built already (by a first call) or not, translated or stale
    if (chance(50)) call_orig(F_async_msg, 0, {1});
    if (chance(50)) call_orig(0x004a76d0, 0, {0});
    if (chance(30)) *(uint32_t*)(uintptr_t)G_XLATOR_COOKIE = rnd();
    *(uint32_t*)(uintptr_t)0x004fb44c = (uint32_t)irange(0, 300);           // MultiGetPacketDelay's
    g_rpp_pool_off = U(&W.rdp->rpp_pool) - U(g_arena);
    g_dq_pool_off = U(&W.dm->q.pool) - U(g_arena);
}

// ---- the frame garbage the originals leave in the world (never compared) -------------------------------------------------------
// Packets built in a frame carry what the frame held where the builder doesn't write (byte 1 of session control, the password's
// tail, a ReliablePacketInfo's last bytes -- init_rpi's 2, Tick's 4 --, a DataModerator record past its packet), and the
// rewrite's frames aren't the original's. Those bytes are left out of the comparison wherever they're copied: every
// slot of the reliable port's packet pool (in use or free) and of the DataModerator's queue.
static void mask_pool(uint8_t* ar, const uint8_t* ref, uint32_t pool_off, void (*slot)(uint8_t*)) {
    const PoolBase* p = (const PoolBase*)(ref + pool_off);
    const uint32_t a = U(g_arena);
    const uint32_t blk = U(p->block);
    if (blk < a || blk >= a + ARENA_BYTES || p->count <= 0 || p->count > 4096 || p->size == 0 || p->size > 0x1000) return;
    for (int i = 0; i < p->count; i++) {
        const uint32_t off = blk - a + (uint32_t)i * p->size;
        if (off + p->size <= ARENA_BYTES) slot(ar + off);
    }
}
static void mask_rpp(uint8_t* r) {
    RPP* p = (RPP*)r;
    if ((p->pkt[0] & 0xf) == 0 && p->pkt[2] == 0x1e) {
        p->pkt[1] = 0;
        if (p->pkt[3] == 0)
            for (int i = 9; i < 25; i++)
                if (!p->pkt[i]) { for (int j = i + 1; j < 25; j++) p->pkt[j] = 0; break; }
    }
    const uint32_t fn = U(p->rpi.fn);
    if (fn == F_sess_rdp_cb) { p->rpi.data[6] = 0; p->rpi.data[7] = 0; }
    if (fn == F_servreq_cb) memset(p->rpi.data + 4, 0, 4);
}
static void mask_dq(uint8_t* e) {
    DQEntry* q = (DQEntry*)e;
    if (q->len != 0xf4) return;
    int32_t n;
    memcpy(&n, q->data + 0xe4, 4);
    if (n >= 0 && n < 0xe4) memset(q->data + n, 0, 0xe4 - n);
}
static void mask_garbage(uint8_t* ar, const uint8_t* ref) {
    if (g_rpp_pool_off) mask_pool(ar, ref, g_rpp_pool_off, mask_rpp);
    if (g_dq_pool_off) {
        uint32_t pp;
        memcpy(&pp, ref + g_dq_pool_off, 4);                     // the DelayQueue's PoolBase*
        const uint32_t a = U(g_arena);
        if (pp >= a && pp < a + ARENA_BYTES - 0x20) mask_pool(ar, ref, pp - a, mask_dq);
    }
}

// ---- arguments per function -------------------------------------------------------------------------------------------------------
static HostEntry* any_host() {
    if (W.nhosts) return W.hosts[rnd() % W.nhosts];
    HostEntry* h = (HostEntry*)(uintptr_t)call_orig(F_RDP_get_entry, 1, {U(W.rdp), 0, U(addr_k(0)), 1});
    if (h) { W.hosts[0] = h; W.nhosts = 1; }
    return h;
}
static RPP* any_rpp(HostEntry* h) {
    RPP* lists[4] = {h ? h->sent : 0, h ? h->waiting : 0, h ? h->held : 0, W.rdp->ready};
    for (int t = 0; t < 4; t++) {
        RPP* l = lists[rnd() % 4];
        int n = 0;
        for (RPP* p = l; p; p = p->next) n++;
        if (n) { int k = (int)(rnd() % n); RPP* p = l; while (k--) p = p->next; return p; }
    }
    return make_rpp((uint8_t)rnd());
}
static int chan_arg() {
    const int ns = W.sm->nsessions;
    if (!ns) return 0;
    return chance(95) ? irange(0, ns - 1) : irange(ns, ns + 1);
}
static uint32_t id_arg() { return W.nlocal_ids && chance(80) ? W.local_ids[rnd() % W.nlocal_ids] : (uint32_t)irange(990, 1010); }
static const void* addr_arg() {
    if (W.sm->nsessions && chance(40)) return &W.sm->sessions[rnd() % W.sm->nsessions].addr;
    return addr_k(rnd());
}
static uint8_t* car_phys_packet() {
    uint8_t* p = sv(0x40);
    float* f = (float*)p;
    f[1] = wildf(0, 2000);                       // time
    f[2] = chance(50) ? 0.0f : wildf(0, 1);       // braking
    f[3] = wildf(-1.2f, 1.2f);                    // steering
    *(int32_t*)(p + 0x14) = irange(-1, 7);        // gear
    f[6] = wildf(0, 9000);                        // rpm
    for (int k = 0; k < 3; k++) *(float*)(p + 0x1c + 4 * k) = wildf(-5000, 5000);
    float q[4];
    for (int k = 0; k < 4; k++) q[k] = frange(-1, 1);
    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int k = 0; k < 4; k++) *(float*)(p + 0x28 + 4 * k) = chance(10) ? wildf(-2, 2) : q[k] / (n > 0 ? n : 1);
    if (chance(10)) *(float*)(p + 0x34) = chance(50) ? 1.0f : -1.0f;
    p[0x38] = (uint8_t)(rnd() & 1); p[0x39] = (uint8_t)(rnd() & 1);
    for (int k = 0; k < 4; k++) p[0x3a + k] = chance(70) ? 0 : (uint8_t)rnd();
    return p;
}
static bool g_calm;                              // this function's world: channels without flags (find_range)
static bool args_for(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 18; i++) w[i] = rnd();
    SessionMgr* m = W.sm;
    uint32_t* a = f.fast ? w + 2 : w;             // the stack arguments
    if (f.fast) w[1] = rnd();
    switch (f.v10) {
    // ---- delqueue / datamod -----------------------------------------------------------------------------------------
    case 0x004a4000: w[0] = U(sv(0x10)); a[0] = (uint32_t)irange(0, 6); a[1] = (uint32_t)irange(0, 80); return true;
    case 0x004a4060: case 0x004a4090: case 0x004a4130:
        w[0] = U(W.dq);
        if (f.v10 == 0x004a4090) { a[0] = U(sv(0x60)); a[1] = (uint32_t)irange(0, W.dq->size); a[2] = rnd(); }
        if (f.v10 == 0x004a4130) {
            int32_t* len = (int32_t*)sv(4);
            *len = chance(80) ? irange(W.dq->size - 4, W.dq->size + 8) : irange(0, 8);
            a[0] = U(sv(0x80)); a[1] = U(len); a[2] = chance(50) ? 0 : U(sv(4));
        }
        return true;
    case 0x004afc70: w[0] = U(sv(0x14)); a[0] = U(W.sock); return true;
    case 0x004afc90: case 0x004afca0: case 0x004afd30: w[0] = U(W.dm); return true;
    case 0x004afcc0: w[0] = U(W.dm); a[0] = U(sv(0xe4)); a[1] = (uint32_t)irange(0, 0xe4); a[2] = U(addr_arg()); return true;
    // ---- DataPort --------------------------------------------------------------------------------------------------
    case 0x004aef60: w[0] = U(sv(0xc)); a[0] = U(W.sock); return true;
    case 0x004aef90: w[0] = U(chance(50) ? (void*)W.dp : (void*)W.rdp); return true;
    case 0x004aefd0: case 0x004af010: {
        w[0] = U(chance(50) ? (void*)W.dp : (void*)W.rdp);
        uint8_t* p = sv(0x80);
        a[0] = U(p); a[1] = (uint32_t)irange(1, 0x80); a[2] = U(addr_arg());
        return true;
    }
    case 0x004aeff0: case 0x004af080: case 0x004af450: {
        w[0] = U(f.v10 == 0x004af450 || chance(60) ? (void*)W.rdp : (void*)W.dp);
        int32_t* len = (int32_t*)sv(4);
        *len = chance(80) ? 0xe2 : irange(0, 0x100);
        a[0] = U(sv(0x100)); a[1] = U(len); a[2] = chance(10) ? 0 : U(sv(12));
        return true;
    }
    case 0x004af040: case 0x004af260: {
        w[0] = U(f.v10 == 0x004af260 ? (void*)W.rdp : (void*)W.dp);
        uint8_t* p = sv(0x100);
        a[1] = (uint32_t)make_packet(p, 0x100);
        a[0] = U(p); a[2] = U(chance(70) ? addr_k(rnd()) : sv(12));
        return true;
    }
    case 0x004af100: case 0x004af110: w[0] = U(W.dp); a[0] = U(sv(12)); return true;
    // ---- ReliableDataPort ----------------------------------------------------------------------------------------------
    case 0x004af130: w[0] = U(sv(0x60)); a[0] = U(W.sock); a[1] = (uint32_t)irange(0, 6); return true;
    case 0x004af1b0: case 0x004af1c0: case 0x004af300: case 0x004af3c0: w[0] = U(W.rdp); return true;
    case 0x004af210: w[0] = U(W.rdp); a[0] = rnd(); a[1] = U(sv(8)); return true;
    case 0x004af230: {
        w[0] = U(W.rdp);
        uint8_t* p = sv(0x60);
        RPI* r = (RPI*)sv(12);
        random_rpi(r);
        a[0] = U(p); a[1] = (uint32_t)irange(2, 0x60); a[2] = U(chance(70) ? addr_k(rnd()) : sv(12)); a[3] = chance(30) ? 0 : U(r);
        return true;
    }
    case 0x004af370: {
        w[0] = U(W.rdp);
        HostEntry* h = any_host();
        if (!h) return false;
        if (chance(30) && W.nhosts > 1) W.rdp->dead = W.hosts[(rnd() % (W.nhosts - 1)) + 1];
        if (chance(30)) W.rdp->timeout_cb = 0;
        a[0] = U(h); a[1] = U(&h->addr);
        return true;
    }
    case 0x004af3e0: w[0] = U(W.rdp); a[0] = U(chance(80) ? addr_k(rnd()) : sv(12)); a[1] = rnd() & 1; return true;
    case 0x004af4e0: w[0] = U(W.rdp); a[0] = U(chance(80) ? addr_k(rnd()) : sv(12)); return true;
    // ---- HostEntry ----------------------------------------------------------------------------------------------------------
    case 0x004affe0: case 0x004b0010: w[0] = U(sv(0x38));
        if (f.v10 == 0x004b0010) { a[0] = U(addr_k(rnd())); a[1] = U(W.rdp); a[2] = U(&W.rdp->ready); }
        return true;
    case 0x004b0070: return true;
    case 0x004b00b0: a[0] = (uint32_t)irange(-1, 7); return true;
    default: break;
    }
    if (f.v10 >= 0x004b0000 && f.v10 <= 0x004b0800) {                // HostEntry's members
        HostEntry* h = any_host();
        if (!h) return false;
        w[0] = U(h);
        switch (f.v10) {
        case 0x004b00c0: case 0x004b0200: a[0] = (uint32_t)(uint8_t)(h->recv_id + irange(-20, 20)); return true;
        case 0x004b00d0: case 0x004b0100: {
            RPP** lists[4] = {&h->sent, &h->waiting, &h->held, &W.rdp->ready};
            RPP* r = make_rpp((uint8_t)(h->recv_id + irange(-2, 8)));
            if (!r) return false;
            a[0] = U(lists[f.v10 == 0x004b0100 ? 2 : rnd() % 4]); a[1] = U(r);
            if (f.v10 == 0x004b0100 && chance(50) && h->held) r->pkt[1] = h->held->pkt[1];
            return true;
        }
        case 0x004b01a0: {
            RPP* r = (RPP*)sv(sizeof(RPP));
            RPI* i = (RPI*)sv(12);
            random_rpi(i);
            a[0] = U(r); a[1] = U(sv(0x80)); a[2] = (uint32_t)irange(0, 0x80); a[3] = chance(40) ? 0 : U(i);
            return true;
        }
        case 0x004b0230: a[0] = U(chance(30) ? (void*)0 : (void*)any_rpp(h)); a[1] = (uint32_t)(uint8_t)(h->recv_id + irange(-3, 6)); return true;
        case 0x004b0260: case 0x004b0610: case 0x004b0680: { RPP* r = make_rpp((uint8_t)rnd()); if (!r) return false; a[0] = U(r); return true; }
        case 0x004b0290: {
            RPI* i = (RPI*)sv(12);
            random_rpi(i);
            a[0] = U(sv(0x60)); a[1] = (uint32_t)irange(2, 0x60); a[2] = chance(30) ? 0 : U(i);
            return true;
        }
        case 0x004b0330: {
            uint8_t* p = sv(2);
            p[0] = (uint8_t)(((rnd() % 2) << 4) | (rnd() & 0xf));
            p[1] = h->sent && chance(70) ? any_rpp(h)->pkt[1] : (uint8_t)rnd();
            if (h->sent && chance(50)) p[1] = h->sent->pkt[1];
            a[0] = U(p);
            return true;
        }
        case 0x004b03f0: a[0] = rnd() & 0xff; a[1] = rnd() % 3; return true;
        case 0x004b0440: { RPP** lists[3] = {&h->sent, &h->waiting, &h->held}; a[0] = U(lists[rnd() % 3]); return true; }
        case 0x004b04b0: {
            uint8_t* p = sv(0x80);
            p[0] = (uint8_t)(((2 + rnd() % 4) << 4) | (rnd() & 0xf));
            p[1] = (uint8_t)(h->recv_id + irange(-18, 6));
            if (h->held && chance(30)) p[1] = h->held->pkt[1];
            a[0] = U(p); a[1] = (uint32_t)irange(2, 0x80);
            return true;
        }
        default: return true;                                   // no arguments (ctor / dtor / destroy / free_all / Tick)
        }
    }
    // ---- nettypes ---------------------------------------------------------------------------------------------------------
    switch (f.v10) {
    case 0x004a4400: {                                          // (past 2^31 after the offset and scaling: the unsigned reload)
        float* p = (float*)sv(12);
        for (int k = 0; k < 3; k++) p[k] = chance(50) ? wildf(-3000, 3000) : frange(-3e6f, 3e6f);
        a[0] = U(p);
        return true;
    }
    case 0x004a44d0: case 0x004a45d0: case 0x004a4600: {
        uint8_t* in = sv(0x20);
        if (chance(30)) { uint16_t z = (uint16_t)(chance(50) ? 0x4000 : rnd()); memcpy(in + 2 + 12 + 2 * (rnd() % 3), &z, 2); }
        if (f.v10 == 0x004a4600 && chance(20)) memset(in, 0x40, 6);       // near the axis' zero: identity
        a[0] = U(sv(0x40));
        a[1] = U(f.v10 == 0x004a44d0 ? in : f.v10 == 0x004a45d0 ? in + 2 : in);
        return true;
    }
    case 0x004a4740: a[0] = rnd() & 0xffff; return true;
    case 0x004a4780: case 0x004a48b0: case 0x004a48e0: {
        uint8_t* in = car_phys_packet();
        a[0] = U(sv(0x20));
        a[1] = U(f.v10 == 0x004a4780 ? in : in + 0x1c);
        if (f.v10 == 0x004a48e0) a[1] = U(in + 0x28);
        return true;
    }
    default: break;
    }
    // ---- session.obj ---------------------------------------------------------------------------------------------------------
    w[0] = U(m);
    switch (f.v10) {
    case 0x004a4d20: w[0] = U(sv(0xd8)); a[0] = U(W.sock); a[1] = (uint32_t)irange(0, 8); a[2] = (uint32_t)irange(0, 5); a[3] = (uint32_t)irange(0, 4); return true;
    case 0x004a7670: a[0] = U(W.sock); a[1] = (uint32_t)irange(0, 8); a[2] = (uint32_t)irange(0, 5); a[3] = (uint32_t)irange(0, 4); return true;
    case 0x004a4ed0: {                                          // sess_rdp_cb(result, RPUserData&)
        uint8_t* ud = sv(8);
        void* p = m; memcpy(ud, &p, 4); ud[4] = (uint8_t)(rnd() % 8); ud[5] = (uint8_t)chan_arg();
        a[0] = rnd() & 0xff; a[1] = U(ud);
        return true;
    }
    case 0x004a4ef0: { uint32_t* gd = (uint32_t*)sv(8); gd[0] = U(m); a[0] = U(addr_arg()); a[1] = U(gd); return true; }
    case 0x004a4f10: a[0] = U(sv(12)); a[1] = rnd() & 0xff; a[2] = rnd() & 0xff; return true;
    case 0x004a5060: a[0] = U(W.blk); return true;
    case 0x004a5320: a[0] = (uint32_t)irange(0, 5); return true;
    case 0x004a76d0: a[0] = (uint32_t)irange(-2, 8); return true;
    case 0x004a5560: {                                          // servreq_cb(result, RPUserData&)
        uint32_t* ud = (uint32_t*)sv(8);
        ud[0] = U(&m->async);
        a[0] = rnd() & 0xff; a[1] = U(ud);
        return true;
    }
    case 0x004a5620: a[0] = chance(70) ? 0x1001 : rnd(); return true;
    case 0x004a5720: { char* n = (char*)sv(16); strcpy(n, "host"); a[0] = U(n); a[1] = rnd() & 0xffff; a[2] = 0x1001; return true; }
    case 0x004a57b0: { char* n = (char*)sv(16); strcpy(n, "host"); a[0] = U(n); a[1] = rnd() & 0xffff; a[2] = 0x1001; a[3] = U(W.blk); return true; }
    case 0x004a5870: case 0x004a58f0: {
        LocalService* l = (LocalService*)sv(0x48);
        l->type = 0x1001;
        const int ns = m->nsessions;
        int lo = ns ? irange(0, ns - 1) : 0, hi = ns ? irange(lo, ns) : 0;
        l->lo = (int16_t)lo; l->hi = (int16_t)hi;
        a[0] = U(l);
        if (f.v10 == 0x004a5870) a[1] = (uint32_t)irange(1, ns + 1);
        return true;
    }
    case 0x004a5a90: case 0x004a5b80: case 0x004a5c20: case 0x004a5cc0: case 0x004a5f70: a[0] = id_arg(); return true;
    case 0x004a5b30: if (!m->nlocal) return false; a[0] = U(&m->local[rnd() % m->nlocal]); return true;
    case 0x004a5d90: {
        RemoteService* r = m->nremote && chance(70) ? &m->remote[rnd() % m->nremote] : (RemoteService*)sv(0x54);
        a[0] = U(r); a[1] = (uint32_t)chan_arg();
        return true;
    }
    case 0x004a5ea0: a[0] = id_arg(); a[1] = (uint32_t)chan_arg(); return true;
    case 0x004a5fe0: a[0] = U(m->nremote && chance(70) ? (void*)&m->remote[rnd() % m->nremote] : (void*)sv(0x54)); return true;
    case 0x004a6050: case 0x004a6670: case 0x004a66e0: case 0x004a75e0: a[0] = (uint32_t)chan_arg(); return true;
    case 0x004a60a0: a[0] = (uint32_t)chan_arg(); a[1] = (uint32_t)irange(0, 8); return true;
    case 0x004a6180: a[0] = id_arg(); a[1] = (uint32_t)irange(0, 8); return true;
    case 0x004a62f0: a[0] = (uint32_t)irange(0, 8); return true;
    case 0x004a6370: case 0x004a6420: {
        a[0] = (uint32_t)chan_arg();
        a[1] = U(sv(0x60)); a[2] = (uint32_t)irange(3, 0x60);
        if (f.v10 == 0x004a6370) a[3] = rnd() & 0xff;
        else { RPI* r = (RPI*)sv(12); random_rpi(r); a[3] = chance(40) ? 0 : U(r); a[4] = rnd() & 0xff; }
        return true;
    }
    case 0x004a64c0: { int32_t* len = (int32_t*)sv(4); *len = rnd(); a[0] = (uint32_t)chan_arg(); a[1] = U(sv(0xe4)); a[2] = U(len); return true; }
    case 0x004a6590: { int32_t* len = (int32_t*)sv(4); *len = rnd(); a[0] = id_arg(); a[1] = U(sv(0xe4)); a[2] = U(len); return true; }
    case 0x004a6760: case 0x004a6850: {
        a[0] = U(sv(4));
        if (f.v10 == 0x004a6760) a[1] = chance(50) ? 0 : id_arg();
        return true;
    }
    case 0x004a6860: {
        LocalService* l = (LocalService*)sv(0x48);
        a[0] = (uint32_t)irange(1, m->nsessions + 1); a[1] = U(l);
        return true;
    }
    case 0x004a6900: case 0x004a6c20: case 0x004a6d60: case 0x004a6e40: case 0x004a6f40: case 0x004a7140: case 0x004a71a0:
    case 0x004a7290: case 0x004a6990: case 0x004a6a90: {
        uint8_t* p = sv(0x100);
        int len = 0;
        for (int t = 0; t < 40; t++) {                          // a packet of the function's kind, mostly
            len = make_packet(p, 0x100);
            const bool ctl = !(p[0] & hg8(G_CHAN_MASK)) && p[2] == 0x1e;
            const uint32_t want = f.v10 == 0x004a6c20 ? 0 : f.v10 == 0x004a6d60 ? 1 : f.v10 == 0x004a6e40 ? 2 : f.v10 == 0x004a6f40 ? 3
                                : f.v10 == 0x004a6900 ? 4 : f.v10 == 0x004a7140 ? 5 : f.v10 == 0x004a71a0 ? 6 : f.v10 == 0x004a7290 ? 7 : 99;
            if (want == 99) { if (f.v10 == 0x004a6990 || !ctl) break; continue; }
            if (ctl && p[3] == want) break;
        }
        const void* ad = addr_arg();
        if (f.v10 == 0x004a6d60 || f.v10 == 0x004a6e40 || f.v10 == 0x004a6f40 || f.v10 == 0x004a6900) {
            const int s = f.v10 == 0x004a6e40 ? p[5] : p[4];
            if (s < m->nsessions && chance(60)) ad = &m->sessions[s].addr;
        }
        if (f.v10 == 0x004a6a90) {
            const int s = p[(((p[0] >> 4) & 0xf) < 7) ? 2 : 1];
            if (s < m->nsessions && chance(60)) ad = &m->sessions[s].addr;
        }
        if (f.v10 == 0x004a71a0 || f.v10 == 0x004a7290) if (m->nremote && chance(50)) ad = &m->remote[rnd() % m->nremote].addr;
        if (f.v10 == 0x004a6990 || f.v10 == 0x004a6a90) { a[0] = U(p); a[1] = (uint32_t)len; a[2] = U(ad); }
        else { a[0] = U(p); a[1] = U(ad); }
        return true;
    }
    case 0x004a7000: { a[0] = U(sv(0x44)); a[1] = U(m->nlocal && chance(80) ? (void*)&m->local[rnd() % m->nlocal] : (void*)sv(0x48)); return true; }
    case 0x004a7100: a[0] = U(m->nlocal && chance(80) ? (void*)&m->local[rnd() % m->nlocal] : (void*)sv(0x48)); return true;
    case 0x004a7320: a[0] = U(addr_arg()); return true;
    case 0x004a73d0: { uint8_t* d = sv(4); d[0] = (uint8_t)(rnd() % 8); d[1] = (uint8_t)chan_arg(); a[0] = U(d); return true; }
    case 0x004a74c0: if (!m->nsessions) return false; a[0] = U(&m->sessions[rnd() % m->nsessions]); return true;
    case 0x004a7530: a[0] = U(addr_arg()); a[1] = rnd() & 0xff; a[2] = (uint32_t)irange(0, 6); return true;
    case 0x004a7570: { char* pw = (char*)sv(24); for (int i = 0; i < 23; i++) pw[i] = (char)('a' + rnd() % 26); pw[irange(0, 23)] = 0; a[0] = U(pw); return true; }
    case 0x004a7650: w[0] = U(&m->dropped); m->dropped = chance(50) ? 0 : (int32_t)rnd(); return true;
    default: return true;                                       // the SessionMgr alone
    }
}

// ---- the leftovers phase (world_leftover.cpp's method) ---------------------------------------------------------------------------
static void poison() {
    uint32_t* d = (uint32_t*)DATA;
    for (uint32_t i = 0; i < DATA_BYTES / 4; i++) d[i] = rnd();
    *(uint32_t*)0x005024b8 = 0;                                   // __adjust_fdiv
}
static void random_arena_part(uint32_t from, uint32_t n) { for (uint32_t i = 0; i < n; i++) g_arena[from + i] = (uint8_t)rnd(); }

// ---- the fix build: rounds a fix changed, and the directed tests ------------------------------------------------------------------
struct FixCount { const char* what; int rounds, differed; };
static FixCount g_fixc[32];
static int g_nfixc;
static void fix_round(const char* what, bool same) {
    int i = 0;
    while (i < g_nfixc && strcmp(g_fixc[i].what, what)) i++;
    if (i == g_nfixc) {
        if (g_nfixc == 32) return;
        g_fixc[g_nfixc++] = {what, 0, 0};
    }
    g_fixc[i].rounds++;
    g_fixc[i].differed += !same;
}
#if NET_FIXES
// directed_fix_tests: each fix's bad case on the original (shown failing: a fault, an overrun, a panic, a hang) and on the
// rewrite (clean: no fault, the callee-saved registers and the stack kept, the result the fix promises), and a boundary case
// that still fits on both, compared bit for bit as the random rounds are.
static int g_dt_bad, g_dt_n;
static void dt_check(bool ok, const char* name, const char* what) {
    g_dt_n++;
    printf("  fix test %-34s %s  %s\n", name, ok ? "ok    " : "FAILED", what);
    if (!ok) g_dt_bad++;
}
static const Ent* ent_at(uint32_t v10) {
    for (int i = 0; i < g_nfns; i++)
        if (!g_fns[i].leftover && g_fns[i].v10 == v10) return &g_fns[i];
    printf("  (no rewrite listed at %08x)\n", v10);
    ExitProcess(5);
}
static void dt_world(bool calm) {
    mem_load(g_pristine);
    memset(g_arena, 0, ARENA_BYTES);
    g_logging = false;
    build_world(calm);
    HS()->fail_mask = 0;
    g_logging = true;
    g_pc = _PC_53;
}
static Mem g_dt_o;                                   // the original's memory after its run
// the original then the rewrite from one snapshot (g_snap); the original's memory and call log kept (g_dt_o, g_log_orig)
static void dt_both(const Ent& f, const uint32_t* w, Result& ro, Result& rn) {
    if (!g_dt_o.data) g_dt_o = mem_alloc();
    mem_save(g_snap);
    ro = run(f, false, w);
    mem_save(g_dt_o);
    g_log_orig = g_log;
    mem_load(g_snap);
    g_fix_hits = 0;
    rn = run(f, true, w);
}
// the two runs alike, as the random rounds compare them
static bool dt_same(const Result& ro, const Result& rn) {
    bool same = ro.fault == rn.fault && ro.code == rn.code && ro.ret == rn.ret && ro.pops == rn.pops &&
                !memcmp(ro.regs, rn.regs, sizeof ro.regs) && ro.top == rn.top;
    mask_garbage(g_dt_o.arena, g_snap.arena);
    mask_garbage(g_arena, g_snap.arena);
    same &= mem_diff(g_dt_o) == 0;
    same &= g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
    return same;
}
// a clean return: no fault, ebx / esi / edi / ebp as raw_call set them, the x87 stack empty, the stack arguments popped
static bool clean(const Ent& f, const Result& r) {
    return !r.fault && r.regs[0] == 0x0b0b0b0b && r.regs[1] == 0x05050505 && r.regs[2] == 0x0d0d0d0d && r.regs[3] == 0x0e0e0e0e &&
           r.top == 0 && r.pops == (f.fast ? 4u * (uint32_t)f.nstack : 0u);
}
static bool log_has(const CallLog& l, uint32_t tag, uint32_t next) {
    for (uint32_t i = 0; i + 1 < l.n && i + 1 < LOG_MAX; i++)
        if (l.w[i] == tag && (next == 0 || l.w[i + 1] == next)) return true;
    return false;
}
// the original on a thread with a time limit: true if it was still running (an endless loop; it only reads, so it is
// simply stopped)
struct HangCall { const Ent* f; const uint32_t* w; Result r; };
static DWORD WINAPI hang_thread(void* p) {
    HangCall* h = (HangCall*)p;
    h->r = run(*h->f, false, h->w);
    return 0;
}
static bool runs_forever(const Ent& f, const uint32_t* w, int ms) {
    HangCall h = {&f, w, {}};
    HANDLE t = CreateThread(0, 0x800000, hang_thread, &h, STACK_SIZE_PARAM_IS_A_RESERVATION, 0);
    const bool hung = WaitForSingleObject(t, (DWORD)ms) == WAIT_TIMEOUT;
    if (hung) TerminateThread(t, 0);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return hung;
}
static int list_len(HostEntry* h) {
    int n = 0;
    for (; h && n < 1000; h = h->next) n++;
    return n;
}
static uint8_t* new_addr() {
    uint8_t* a = sv(12);
    a[0] = 2; a[1] = 0;
    return a;
}
static HostEntry* orig_get_entry(const uint8_t* a) { return (HostEntry*)(uintptr_t)call_orig(F_RDP_get_entry, 1, {U(W.rdp), 0, U(a), 1}); }
// the reliable port's host pool used up by new hosts (each idle: no packets); `busy`: then each host given a sent packet
static void fill_hosts(bool busy) {
    g_logging = false;
    for (int k = 0; k < 64 && W.rdp->node_pool.free_list; k++) orig_get_entry(new_addr());
    if (busy)
        for (HostEntry* h = W.rdp->hosts; h; h = h->next)
            if (!h->sent && !h->waiting && !h->held) {
                RPP* d = (RPP*)sv(sizeof(RPP));                        // (only its being there counts)
                d->next = 0;
                h->sent = d;
            }
    g_logging = true;
}

static int directed_fix_tests() {
    printf("directed fix tests (the bad case on the original, then on the rewrite; the boundary case on both, compared):\n");
    uint32_t w[18];
    Result ro, rn;
    char msg[256];

    // ---- DataModerator::Send: a packet longer than the record ----------------------------------------------------------
    {
        const Ent& f = *ent_at(0x004afcc0);
        dt_world(false);
        uint8_t* pk = sv(0x100);
        memset(pk, 0xcc, 0x100);
        memset(w, 0, sizeof w);
        w[0] = U(W.dm); w[2] = U(pk); w[3] = 0x100; w[4] = U(addr_k(1));        // (0x100: up to the return address)
        dt_both(f, w, ro, rn);
        dt_check(ro.fault && clean(f, rn) && mem_diff(g_snap) == 0 && g_fix_hits, "DataModerator::Send",
                 "0x100 bytes: the original overran its frame (a fault); the rewrite drops it, nothing queued");
        dt_world(false);
        uint8_t* pk2 = sv(0xe4);
        w[0] = U(W.dm); w[2] = U(pk2); w[3] = 0xe4; w[4] = U(addr_k(1));
        dt_both(f, w, ro, rn);
        dt_check(dt_same(ro, rn) && !g_fix_hits, "DataModerator::Send", "0xe4 bytes: both alike");
    }
    // ---- HostEntry::init_rpp: a length past the packet -----------------------------------------------------------------
    {
        const Ent& f = *ent_at(0x004b01a0);
        dt_world(false);
        HostEntry* h = any_host();
        uint8_t* r = sv(0x300);
        memset(r, 0, 0x300);
        uint8_t* pk = sv(0x200);
        memset(pk, 0xa7, 0x200);
        memset(w, 0, sizeof w);
        w[0] = U(h); w[2] = U(r); w[3] = U(pk); w[4] = 0x200; w[5] = 0;
        dt_both(f, w, ro, rn);
        const uint32_t ro_off = U(r) - U(g_arena);
        bool over = true, kept = true;
        for (uint32_t i = 0x104; i < 0x200; i++) {
            over &= g_dt_o.arena[ro_off + i] == 0xa7;
            kept &= r[i] == 0;
        }
        dt_check(over && clean(f, rn) && kept && ((RPP*)r)->len == 0xe4 && !memcmp(r, pk, 0xe4), "HostEntry::init_rpp",
                 "0x200 bytes: the original wrote 0xfc past the 0x104-byte packet; the rewrite keeps 0xe4 of them");
        dt_world(false);
        h = any_host();
        r = sv(0x300);
        w[0] = U(h); w[2] = U(r); w[3] = U(sv(0xe4)); w[4] = 0xe4; w[5] = 0;
        dt_both(f, w, ro, rn);
        dt_check(dt_same(ro, rn) && !g_fix_hits, "HostEntry::init_rpp", "0xe4 bytes: both alike");
    }
    // ---- ReliableDataPort::get_entry: the host pool used up ------------------------------------------------------------
    {
        const Ent& f = *ent_at(0x004af3e0);
        dt_world(false);
        fill_hosts(false);
        HostEntry* idle = 0;
        for (HostEntry* h = W.rdp->hosts; h && !idle; h = h->next)
            if (!h->sent && !h->waiting && !h->held) idle = h;
        const int n0 = list_len(W.rdp->hosts);
        uint8_t* a = new_addr();
        memset(w, 0, sizeof w);
        w[0] = U(W.rdp); w[2] = U(a); w[3] = 1;
        dt_both(f, w, ro, rn);
        HostEntry* got = (HostEntry*)(uintptr_t)rn.ret;
        int on = 0;
        for (HostEntry* h = W.rdp->hosts; h; h = h->next) on += h == got;
        sprintf(msg, "a host past the pool's %d (one idle): the original panicked (\"overalloc\"); the rewrite reuses the idle one",
                W.rdp->node_pool.count);
        dt_check(log_has(g_log_orig, 'LPAN', 0) && ro.ret == 0 && clean(f, rn) && !log_has(g_log, 'LPAN', 0) && got == idle &&
                     !memcmp(&got->addr, a, 12) && on == 1 && list_len(W.rdp->hosts) == n0 && got->rtt_n == 1 && got->send_first &&
                     got->recv_first && !got->next,
                 "ReliableDataPort::get_entry", msg);
        dt_world(false);
        fill_hosts(true);
        a = new_addr();
        w[0] = U(W.rdp); w[2] = U(a); w[3] = 1;
        dt_both(f, w, ro, rn);
        dt_check(log_has(g_log_orig, 'LPAN', 0) && clean(f, rn) && rn.ret == 0 && !log_has(g_log, 'LPAN', 0),
                 "ReliableDataPort::get_entry", "a host past the pool, none idle: the original panicked; the rewrite finds none (0)");
        dt_world(false);
        g_logging = false;
        while (W.rdp->node_pool.free_list && ((PoolBase*)&W.rdp->node_pool)->used + 1 < W.rdp->node_pool.count) orig_get_entry(new_addr());
        g_logging = true;
        a = new_addr();
        w[0] = U(W.rdp); w[2] = U(a); w[3] = 1;
        dt_both(f, w, ro, rn);
        dt_check(dt_same(ro, rn) && !g_fix_hits && rn.ret != 0, "ReliableDataPort::get_entry", "the pool's last host: both alike");
    }
    // ---- ReliableDataPort::handle_packet / SendReliable with no host -----------------------------------------------------
    {
        const Ent& fh = *ent_at(0x004af260);
        const Ent& fs = *ent_at(0x004af230);
        for (int cls = 0; cls < 2; cls++) {
            dt_world(false);
            fill_hosts(true);
            uint8_t* p = sv(0x40);
            p[0] = (uint8_t)(cls ? 0x20 : 0x00);
            p[1] = 7;
            memset(w, 0, sizeof w);
            w[0] = U(W.rdp); w[2] = U(p); w[3] = cls ? 0x20 : 2; w[4] = U(new_addr());
            dt_both(fh, w, ro, rn);
            dt_check(ro.fault && clean(fh, rn) && rn.ret == 1, "ReliableDataPort::handle_packet",
                     cls ? "reliable data from a host past the pool: the original faulted (HoldIncomingPacket on 0); the rewrite drops it"
                         : "an ack from a host past the pool: the original faulted (RecvAck on 0); the rewrite drops it");
        }
        dt_world(false);
        fill_hosts(true);
        RPI* rpi = (RPI*)sv(12);
        rpi->fn = 0;
        memset(w, 0, sizeof w);
        w[0] = U(W.rdp); w[2] = U(sv(0x40)); w[3] = 0x20; w[4] = U(new_addr()); w[5] = U(rpi);
        dt_both(fs, w, ro, rn);
        dt_check(ro.fault && clean(fs, rn) && log_has(g_log, 'LREP', 0x004fe7f8), "ReliableDataPort::SendReliable",
                 "to a host past the pool: the original faulted (HostEntry::SendReliable on 0); the rewrite logs and drops it");
        dt_world(false);
        HostEntry* h = any_host();
        uint8_t* p = sv(0x40);
        p[0] = 0x00; p[1] = h && h->sent ? h->sent->pkt[1] : 3;
        w[0] = U(W.rdp); w[2] = U(p); w[3] = 2; w[4] = U(addr_k(0));
        dt_both(fh, w, ro, rn);
        dt_check(dt_same(ro, rn) && !g_fix_hits, "ReliableDataPort::handle_packet", "an ack from a known host: both alike");
    }
    // ---- return_slot(-1) ------------------------------------------------------------------------------------------------
    {
        const Ent& f = *ent_at(0x004b00b0);
        dt_world(false);
        *(volatile uint8_t*)(uintptr_t)(G_HOST_SLOTS - 1) = 0x5a;
        memset(w, 0, sizeof w);
        w[0] = 0xffffffffu;
        dt_both(f, w, ro, rn);
        dt_check(g_dt_o.data[G_HOST_SLOTS - 1 - 0x004e1000] == 0 && clean(f, rn) && mem_diff(g_snap) == 0, "return_slot",
                 "-1 (get_slot's none): the original cleared the byte before the slots; the rewrite writes nothing");
        dt_world(false);
        w[0] = 7;
        dt_both(f, w, ro, rn);
        dt_check(dt_same(ro, rn) && !g_fix_hits, "return_slot", "7: both alike");
    }
    // ---- rtt_n 0: HostEntry::Tick, GetEstRTLatency ------------------------------------------------------------------------
    {
        const Ent& ft = *ent_at(0x004b06e0);
        const Ent& fl = *ent_at(0x004af4e0);
        for (int k = 0; k < 2; k++) {
            dt_world(false);
            HostEntry* h = any_host();
            h->sent = h->waiting = h->held = 0;
            h->rtt_n = k ? 1 : 0;
            h->rtt_sum = 600;
            memset(w, 0, sizeof w);
            w[0] = U(h);
            dt_both(ft, w, ro, rn);
            if (!k) dt_check(ro.fault && ro.code == EXCEPTION_INT_DIVIDE_BY_ZERO && clean(ft, rn), "HostEntry::Tick",
                             "rtt_n 0: the original divided by zero; the rewrite takes the sum as the mean");
            else dt_check(dt_same(ro, rn) && !g_fix_hits, "HostEntry::Tick", "rtt_n 1: both alike");
            dt_world(false);
            h = any_host();
            h->rtt_n = k ? 1 : 0;
            h->rtt_sum = 600;
            w[0] = U(W.rdp); w[2] = U(&h->addr);
            dt_both(fl, w, ro, rn);
            if (!k) dt_check(ro.fault && ro.code == EXCEPTION_INT_DIVIDE_BY_ZERO && clean(fl, rn) && rn.ret == 600,
                             "ReliableDataPort::GetEstRTLatency", "rtt_n 0: the original divided by zero; the rewrite gives the sum");
            else dt_check(dt_same(ro, rn) && !g_fix_hits, "ReliableDataPort::GetEstRTLatency", "rtt_n 1: both alike");
        }
    }
    // ---- ReliableDataPort::Tick: a host going down with another after it ----------------------------------------------------
    {
        const Ent& f = *ent_at(0x004af300);
        for (int k = 0; k < 2; k++) {
            RPP* r;
            do {
                dt_world(false);
                g_logging = false;
                r = make_rpp(0);
                g_logging = true;
            } while (!r);
            W.rdp->hosts = 0;
            W.rdp->dead = 0;
            W.rdp->timeout_cb = 0;
            W.rdp->dp.mod = 0;
            g_logging = false;
            HostEntry* hs[3];
            for (int i = 0; i < 3; i++) hs[i] = orig_get_entry(new_addr());
            g_logging = true;
            HostEntry* down = hs[k ? 2 : 1];                           // the middle one (or, the boundary, the last)
            r->pkt[1] = down->send_id;
            r->sends = 6;
            r->sent = HS()->now - 100000;
            r->rpi.fn = 0;
            down->sent = r;
            memset(w, 0, sizeof w);
            w[0] = U(W.rdp);
            dt_both(f, w, ro, rn);
            if (!k) {
                const SessionMgr* m = (const SessionMgr*)(g_dt_o.arena + (U(W.sm) - U(g_arena)));
                const uint32_t ho = U(m->rdp.hosts);
                const HostEntry* a = ho ? (const HostEntry*)(g_dt_o.arena + (ho - U(g_arena))) : 0;
                const bool lost = a && a->next == 0;                       // the original's list: the first host alone
                dt_check(lost && clean(f, rn) && W.rdp->hosts == hs[0] && hs[0]->next == hs[2] && !hs[2]->next && !W.rdp->dead,
                         "ReliableDataPort::Tick", "the middle of three hosts goes down: the original lost the third from the "
                         "list; the rewrite keeps it");
            } else {
                dt_check(dt_same(ro, rn) && !g_fix_hits && W.rdp->hosts == hs[0] && hs[0]->next == hs[1] && !hs[1]->next,
                         "ReliableDataPort::Tick", "the last of three goes down: both alike");
            }
        }
    }
    // ---- SessionMgr::find_range: a channel in use before the free ones -----------------------------------------------------
    {
        const Ent& f = *ent_at(0x004a6860);
        const Ent& fo = *ent_at(0x004a5870);
        for (int k = 0; k < 2; k++) {
            do dt_world(true); while (W.sm->nsessions < 3 || W.sm->nlocal_max < 1);
            SessionMgr* m = W.sm;
            for (int i = 0; i < m->nsessions; i++) { m->sessions[i].flags = 0; m->sessions[i].service = 0; }
            if (!k) m->sessions[0].flags = 0x81;
            LocalService* ls = (LocalService*)sv(0x48);
            memset(w, 0, sizeof w);
            w[0] = U(m); w[2] = 2; w[3] = U(ls);
            if (!k) {
                mem_save(g_snap);
                const bool hung = runs_forever(f, w, 300);
                mem_load(g_snap);
                g_fix_hits = 0;
                rn = run(f, true, w);
                dt_check(hung && clean(f, rn) && rn.ret == 1 && ls->lo == 1 && ls->hi == 3, "SessionMgr::find_range",
                         "2 channels with channel 0 in use: the original never returned; the rewrite finds 1..2");
                // and OfferUnboundService over it (find_range the rewrite too: hooked for this call)
                mem_load(g_snap);
                m->nlocal = 0;
                LocalService* l2 = (LocalService*)sv(0x48);
                memset(l2, 0, 0x48);
                w[2] = U(l2); w[3] = 2;
                patch_jmp(0x004a6860, f.fn, true);
                rn = run(fo, true, w);
                unpatch_remembered();
                dt_check(clean(fo, rn) && rn.ret != 0 && m->nlocal == 1 && m->sessions[1].service == rn.ret &&
                             m->sessions[2].service == rn.ret && m->sessions[0].service == 0,
                         "SessionMgr::OfferUnboundService", "the same: the service gets channels 1..2");
            } else {
                dt_both(f, w, ro, rn);
                dt_check(dt_same(ro, rn) && !g_fix_hits && rn.ret == 1 && ls->lo == 0 && ls->hi == 2, "SessionMgr::find_range",
                         "every channel free: both alike");
            }
        }
    }
    // ---- SessionMgr::Recv: the channel's packet not on the global list ---------------------------------------------------
    {
        const Ent& f = *ent_at(0x004a64c0);
        for (int k = 0; k < 2; k++) {
            do dt_world(false); while (W.sm->nsessions < 1);
            SessionMgr* m = W.sm;
            m->task = HS()->task_cur;
            m->data = 0;
            for (int i = 0; i < m->nsessions; i++) m->sessions[i].queue = 0;
            SessionData* e = (SessionData*)(uintptr_t)call_orig(F_PoolBase_alloc, 1, {U(&m->data_pool), 0});
            e->next = 0; e->snext = 0; e->sess = 0; e->len = 0x20;
            for (int i = 0; i < 0xe4; i++) e->data[i] = (uint8_t)i;
            m->sessions[0].flags = 1;
            m->sessions[0].queue = e;
            if (k) m->data = e;                                            // (the boundary: on both lists)
            int32_t* len = (int32_t*)sv(4);
            *len = 0xe4;
            uint8_t* buf = sv(0xe4);
            memset(w, 0, sizeof w);
            w[0] = U(m); w[2] = 0; w[3] = U(buf); w[4] = U(len);
            dt_both(f, w, ro, rn);
            if (!k) dt_check(ro.fault && clean(f, rn) && rn.ret == 1 && *len == 0x20 && !memcmp(buf, e->data, 0x20) &&
                                 m->sessions[0].queue == 0 && m->data == 0,
                             "SessionMgr::Recv", "a packet on its channel's list only: the original went through 0; the "
                             "rewrite hands it out");
            else dt_check(dt_same(ro, rn) && !g_fix_hits && rn.ret == 1, "SessionMgr::Recv", "on both lists: both alike");
        }
    }
    // ---- SessionMgr::GetNextStatus: no channels ------------------------------------------------------------------------
    {
        const Ent& f = *ent_at(0x004a6760);
        for (int k = 0; k < 2; k++) {
            do dt_world(false); while (W.sm->nsessions < 1);
            SessionMgr* m = W.sm;
            if (!k) m->nsessions = 0;
            uint8_t* out = sv(4);
            memset(w, 0, sizeof w);
            w[0] = U(m); w[2] = U(out); w[3] = 0;
            dt_both(f, w, ro, rn);
            if (!k) dt_check(ro.fault && ro.code == EXCEPTION_INT_DIVIDE_BY_ZERO && clean(f, rn) && rn.ret == 0xffffffffu,
                             "SessionMgr::GetNextStatus", "no channels: the original divided by zero; the rewrite finds none (-1)");
            else dt_check(dt_same(ro, rn) && !g_fix_hits, "SessionMgr::GetNextStatus", "channels: both alike");
        }
    }
    // ---- SessionMgr::recv_chandata: a packet longer than SessionData's 0xe4 bytes ---------------------------------------
    {
        const Ent& f = *ent_at(0x004a6a90);
        for (int k = 0; k < 2; k++) {
            do dt_world(false); while (W.sm->nsessions < 1);
            SessionMgr* m = W.sm;
            m->sessions[0].flags = 1;
            const int len = k ? 0xe4 : 0x200;
            uint8_t* p = sv(0x200);
            memset(p, 0xa7, 0x200);
            p[0] = 0x71;                                                   // channel data of class 7: the channel in byte 1
            p[1] = 0;
            SessionData* next_free = (SessionData*)m->data_pool.free_list;
            SessionData* const data0 = m->data;
            SessionData* const q0 = m->sessions[0].queue;
            const int used0 = m->data_pool.used;
            memset(w, 0, sizeof w);
            w[0] = U(m); w[2] = U(p); w[3] = (uint32_t)len; w[4] = U(&m->sessions[0].addr);
            dt_both(f, w, ro, rn);
            if (!k) {
                bool over = next_free != 0;
                if (over) {
                    const uint32_t off = U(next_free) - U(g_arena);
                    for (uint32_t i = 0xf4; i < 0x10 + 0x200; i++) over &= g_dt_o.arena[off + i] == 0xa7;
                }
                dt_check(over && clean(f, rn) && m->data_pool.used == used0 && m->data == data0 && m->sessions[0].queue == q0,
                         "SessionMgr::recv_chandata", "0x200 bytes: the original wrote past its pool entry (0x12c bytes "
                         "over); the rewrite drops the packet");
            } else {
                dt_check(dt_same(ro, rn) && !g_fix_hits, "SessionMgr::recv_chandata", "0xe4 bytes: both alike");
            }
        }
    }
    // ---- async_msg: a state outside 0..5 ---------------------------------------------------------------------------------
    {
        const Ent& f = *ent_at(0x004a5320);
        dt_world(false);
        memset(w, 0, sizeof w);
        w[0] = 9;
        dt_both(f, w, ro, rn);
        uint32_t texts[6];
        bool in = false;
        for (int i = 0; i < 6; i++) { texts[i] = *(uint32_t*)(uintptr_t)(net_session::k_async_x[i].obj + 4); in |= ro.ret == texts[i]; }
        dt_check(!in && clean(f, rn) && rn.ret == texts[0], "async_msg",
                 "state 9: the original read past its six texts; the rewrite gives the first");
        dt_world(false);
        w[0] = 5;
        dt_both(f, w, ro, rn);
        dt_check(dt_same(ro, rn) && !g_fix_hits, "async_msg", "state 5: both alike");
    }
    printf("directed fix tests: %d checks, %d failed\n", g_dt_n, g_dt_bad);
    return g_dt_bad;
}
#endif

// ---- main ---------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    const int rounds = argc > 1 ? atoi(argv[1]) : 8;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const bool trace = GetEnvironmentVariableA("VP_TRACE", 0, 0) != 0;
    char only[256] = "";
    GetEnvironmentVariableA("VP_ONLY", only, sizeof only);
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_net_core.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);         // _amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);         // _NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);         // _FF_MSGBANNER
    patch_jmp(0x004d5c10, (void*)&stub_crt_fatal);         // _fptrap
    patch_jmp(0x004140e0, (void*)&stub_MemAlloc);
    patch_jmp(0x00414300, (void*)&stub_MemFree);
    patch_jmp(0x00414390, (void*)&stub_delete);
    patch_jmp(0x00411150, (void*)&stub_LogReport);
    patch_jmp(0x004112b0, (void*)&stub_LogPanic);
    patch_jmp(0x00413b40, (void*)&stub_PTimeNow);
    patch_jmp(0x0041b6e0, (void*)&stub_Random);
    patch_jmp(0x00414dd0, (void*)&stub_TaskGetID);
    patch_jmp(0x00414ec0, (void*)&stub_TaskGetName);
    patch_jmp(0x0041afb0, (void*)&stub_xlate);
    patch_jmp(0x004ceff0, (void*)&stub_atexit);
    patch_jmp(0x004cf0a0, (void*)&stub_sprintf);
    patch_jmp(0x004da350, (void*)&stub_stricmp);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    HS()->brk = A_HEAP;
    // the library's $E initialisers, once (the originals)
    int ne = 0;
    g_logging = false;
    for (int i = 0; i < g_nfns; i++)
        if (g_fns[i].leftover && g_fns[i].name[0] == '$' && g_fns[i].name[1] == 'E') { call_orig(g_fns[i].v10, 0, {}); ne++; }
    g_logging = true;
    printf("ran the multi library's %d $E initialisers (class masks %02x %02x %02x)\n", ne, hg8(G_CLASS_MASK_DP), hg8(G_CLASS_MASK_HE),
           hg8(G_CHAN_MASK));
    mem_save(g_pristine);

    int dup = 0;
    for (int i = 0; i < g_nfns; i++)
        for (int j = i + 1; j < g_nfns; j++)
            if (g_fns[i].v10 == g_fns[j].v10 || !strcmp(g_fns[i].name, g_fns[j].name)) {
                printf("  listed twice: %08x %s / %08x %s\n", g_fns[i].v10, g_fns[i].name, g_fns[j].v10, g_fns[j].name);
                dup++;
            }

    static Footprint fp;
    long long checks = 0, log_words = 0, faulted = 0;
    int differ = 0, fp_bad = 0, bad_fns = 0, hand_n = 0, left_n = 0, skipped = 0;
    for (int phase = 0; phase < 2; phase++) {
        if (phase == 0) {                                        // the leftovers: their destructors stubbed
#define DS(a) patch_jmp(a, (void*)&stub_dtor<a>, true);
            DS(0x0041bac0) DS(0x004a7fb0) DS(0x004aac30) DS(0x004aef90) DS(0x004af1c0)
#undef DS
        } else {
            unpatch_remembered();
            patch_jmp(0x004b0800, (void*)&stub_FOOD);
            patch_jmp(0x004a5050, (void*)&stub_ASSERT);
            patch_jmp(0x004b0190, (void*)&stub_ASSERT);
        }
        for (int fi = 0; fi < g_nfns; fi++) {
            const Ent& f = g_fns[fi];
            if (f.leftover != (phase == 0)) continue;
            if (only[0] && !strstr(f.name, only)) continue;
            (f.leftover ? left_n : hand_n)++;
            const int n = f.leftover ? rounds * 4 : rounds * 40;
            bool fn_bad = false;
            int fn_checks = 0, fn_faults = 0;
            uint32_t shapes[64];                                   // distinct call-log shapes seen (VP_TRACE: coverage)
            int nshapes = 0;
            g_calm = f.v10 == 0x004a6860 || f.v10 == 0x004a5870;
            for (int rd = 0; rd < n && !fn_bad; rd++) {
                g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
                mem_load(g_pristine);
                uint32_t words[18];
                if (f.leftover) {
                    if (rd & 1) poison();
                    random_arena_part(A_SCRATCH, 0x10000);
                    for (int i = 0; i < 18; i++) words[i] = rnd();
                    if (f.fast) words[0] = U(g_arena + A_SCRATCH + 0x100);
                    if (strstr(f.name, "deleting destructor") && (rnd() & 1)) words[2] &= ~1u;
                } else {
                    memset(g_arena, 0, ARENA_BYTES);
                    g_logging = false;
                    build_world(g_calm);
                    const bool ok = args_for(f, words);
                    g_logging = true;
                    if (!ok) { skipped++; continue; }
                }
                fp.n = 0; fp.replay_only = 0; fp.pure = false;
                f.fp(fp, words);
                mem_save(g_snap);
                const Result ro = run(f, false, words);
                // the footprint (not for replay_only): every byte changed lies in it (the harness state excepted)
                if (!fp.replay_only) {
                    uint32_t at = 0;
                    for (uint32_t i = 0; i < DATA_BYTES && !at; i++)
                        if (g_snap.data[i] != DATA[i]) {
                            bool in = false;
                            for (int k = 0; k < fp.n; k++) in |= DATA + i >= (uint8_t*)fp.r[k].p && DATA + i < (uint8_t*)fp.r[k].p + fp.r[k].n;
                            if (!in) at = 0x004e1000 + i;
                        }
                    for (uint32_t i = sizeof(HState); i < ARENA_BYTES && !at; i++)
                        if (g_snap.arena[i] != g_arena[i]) {
                            bool in = false;
                            for (int k = 0; k < fp.n; k++) in |= g_arena + i >= (uint8_t*)fp.r[k].p && g_arena + i < (uint8_t*)fp.r[k].p + fp.r[k].n;
                            if (!in) at = U(g_arena + i);
                        }
                    if (at) {
                        printf("  FOOTPRINT %08x %s: the original changed %08x outside it (round %d)\n", f.v10, f.name, at, rd);
                        fp_bad++; fn_bad = true;
                    }
                }
                mem_save(g_after);
                g_log_orig = g_log;
                {
                    uint32_t h = 2166136261u;                       // the log's shape: its tags, not their values
                    for (uint32_t i = 0; i < g_log.n && i < LOG_MAX; i++) {
                        const uint32_t w = g_log.w[i];
                        if ((w >> 24) >= 'A' && (w >> 24) <= 'Z' && ((w >> 16) & 0xff) >= 'A' && ((w >> 16) & 0xff) <= 'Z')
                            h = (h ^ w) * 16777619u;
                    }
                    h ^= (uint32_t)ro.fault;
                    bool seen = false;
                    for (int i = 0; i < nshapes; i++) seen |= shapes[i] == h;
                    if (!seen && nshapes < 64) shapes[nshapes++] = h;
                }
                mem_load(g_snap);
                g_fix_hits = 0;
                const Result rn = run(f, true, words);
                const int hits = g_fix_hits;
                checks++; fn_checks++;
                log_words += g_log_orig.n;
                if (ro.fault) { faulted++; fn_faults++; }
                bool same = ro.fault == rn.fault && ro.code == rn.code && ro.ret == rn.ret && ro.pops == rn.pops &&
                            !memcmp(ro.regs, rn.regs, sizeof ro.regs) && ro.top == rn.top;
                if (!f.leftover) {
                    mask_garbage(g_after.arena, g_snap.arena);
                    mask_garbage(g_arena, g_snap.arena);
                }
                const uint32_t where = mem_diff(g_after);
                same &= where == 0;
                same &= g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
                if (NET_FIXES && hits) {                            // a fix changed this round: counted, the rewrite clean
                    fix_round(g_fix_what, same);
                    if (rn.fault) {
                        printf("  %08x %s: round %d, a fixed case (%s): the rewrite faulted (%08x at %08x)\n", f.v10, f.name, rd,
                               g_fix_what, rn.code, rn.eip);
                        differ++;
                        fn_bad = true;
                    }
                    continue;
                }
                if (!same) {
                    printf("  MISMATCH %08x %s (round %d, pc %s)\n", f.v10, f.name, rd, g_pc == _PC_24 ? "24" : "53");
                    printf("    fault %d/%d (%08x at %08x / %08x at %08x), return %08x / %08x, popped %u / %u, x87 top %u / %u, "
                           "ebx esi edi ebp %08x %08x %08x %08x / %08x %08x %08x %08x\n",
                           ro.fault, rn.fault, ro.code, ro.eip, rn.code, rn.eip, ro.ret, rn.ret, ro.pops, rn.pops, ro.top, rn.top,
                           ro.regs[0], ro.regs[1], ro.regs[2], ro.regs[3], rn.regs[0], rn.regs[1], rn.regs[2], rn.regs[3]);
                    if (where) {
                        const uint32_t a = U(g_arena);
                        if (where >= a && where < a + ARENA_BYTES) {
                            printf("    memory first differs at arena+%05x: %02x / %02x", where - a, g_after.arena[where - a], g_arena[where - a]);
                            if (!f.leftover) {
                                const PoolBase* rp = (const PoolBase*)(g_snap.arena + g_rpp_pool_off);
                                const uint32_t b = U(rp->block);
                                if (where >= b && where < b + (uint32_t)rp->count * rp->size)
                                    printf(" (the packet pool's slot %u, +%03x)", (where - b) / rp->size, (where - b) % rp->size);
                                const SessionMgr* sm = (const SessionMgr*)(g_snap.arena + (U(W.sm) - a));
                                if (where >= U(sm->sessions) && where < U(sm->sessions) + 0x1c * (uint32_t)sm->nsessions)
                                    printf(" (channel %u, +%02x)", (where - U(sm->sessions)) / 0x1c, (where - U(sm->sessions)) % 0x1c);
                                if (where >= U(W.sm) && where < U(W.sm) + 0xd8) printf(" (SessionMgr +%02x)", where - U(W.sm));
                            }
                            printf("\n");
                        }
                        else printf("    memory first differs at %08x\n", where);
                    }
                    if (g_log.n != g_log_orig.n) printf("    the stub logs differ: %u / %u words\n", g_log_orig.n, g_log.n);
                    for (uint32_t i = 0; i < g_log.n && i < g_log_orig.n && i < LOG_MAX; i++)
                        if (g_log.w[i] != g_log_orig.w[i]) {
                            printf("    the stub logs differ at word %u: %08x / %08x (", i, g_log_orig.w[i], g_log.w[i]);
                            for (uint32_t k = i > 4 ? i - 4 : 0; k <= i; k++) printf(" %08x", g_log_orig.w[k]);
                            printf(" )\n");
                            break;
                        }
                    differ++;
                    fn_bad = true;
                }
            }
            bad_fns += fn_bad;
            if (trace) printf("%08x %-52s %s  %d checks, %d faulted, %d call-log shapes\n", f.v10, f.name, fn_bad ? "BAD" : "ok",
                              fn_checks, fn_faults, nshapes);
            if (differ >= 40) { printf("stopping after 40 differences\n"); goto done; }
        }
    }
done:
    mem_load(g_pristine);
    printf("%d functions (%d hand-written, %d generated), %d listed twice; %lld checks (%d skipped: no object for it), %lld "
           "logged words compared, %lld checks where the original faulted (both alike unless counted below): %d differ, "
           "%d footprint violations; %d functions bad\n", hand_n + left_n, hand_n, left_n, dup, checks, skipped, log_words,
           faulted, differ, fp_bad, bad_fns);
    int fix_bad = 0;
#if NET_FIXES
    printf("fix build: rounds a fix changed (the rewrite clean in each; how many of them differed from the original):");
    for (int i = 0; i < g_nfixc; i++) printf("%s %s %d (%d)", i ? ";" : "", g_fixc[i].what, g_fixc[i].rounds, g_fixc[i].differed);
    printf("%s\n", g_nfixc ? "" : " none");
    if (!only[0]) fix_bad = directed_fix_tests();
#endif
    return differ || fp_bad || dup || fix_bad ? 1 : 0;
}
