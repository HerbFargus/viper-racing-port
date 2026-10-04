// world_net_client.cpp -- multiplayer stage N2, group A: every rewrite of hook/net_client.cpp (client.obj) and
// hook/net_multi.cpp (multi.obj) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_net_client.cpp
//        /Fo<dir>\ /Fe<dir>\world_net_client.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//        /STACK:0x800000,0x800000
//   run:   world_net_client.exe [rounds] [seed]     (VP_TRACE=1: one line per function, with its call-log shapes;
//          VP_ONLY=text: those only; VP_DEBUG_WORLD=1: every fault's registers)
//
// test/world_net_core.cpp's method: out\race_v10.exe loaded at 0x400000 in a child process, the two files included with
// PORT_FN redefined to list each function (VP_FAITHFUL), the multi library's $E initialisers run once (net_leftover.cpp's
// list: the class mask, the static Xlators). Then, for each function, `rounds` x 40 times: a world BUILT in a 1 MB arena --
// for client.obj, a RaceClient made by the original CreateRaceClient (one of its three) over a fake SessionMgr, then made
// random: its state (0..0x16), channel, user id, users (ids whose low byte is their slot, names, statuses), the user list,
// the cars (users, info, LocalCar / NetCar pointers, the human byte), the car requests, a chat ring grown by the original
// dispatch_ChatPacket (past the pool's 40 lines at times), the proposal and race info, the clocks, the server search (by name
// or id, over a table of found services), the track versions, the owner task; for multi.obj, a LiveMultiInfo over a fake
// IRaceClient (a scripted state that its Tick moves on), a fake IRaceServer (or none) and the fake SessionMgr, and the
// statics (the live game or none, world live, escape, autoend, restart, the lobby / trouble flags, the packet delay) --, a
// random harness state (the clock and its step, the task, which allocations fail, the session's answers: GetStatus,
// GetDiscReason, UnboundConnect, Ok, LinkAlive, the latency; the packets Recv hands out -- every game packet type the client
// dispatches, with ids that hit, miss and run past the tables; car packets of 0..8 cars --; the deviation (NaN at times),
// the game state, pause, the car manager, the deity's finished cars, the hornball, the lobby task's suspension / death /
// suspend requests, the screen grabs that fail, CanDestroySafely, EveryoneSynchronized), and the function's arguments made
// from it. The stack is filled with one pattern first, so a packet's bytes the builder never writes are the same in both
// passes and compared too. The ORIGINAL runs; its results kept, the snapshot restored; the REWRITE runs. Compared:
// .data/.bss/.idata and the arena, the return value, bytes popped, ebx / esi / edi / ebp, the x87 depth, faults, and every
// stub's call log (every send by its bytes). A function that isn't replay_only may change only its footprint. x87 at 24 or
// 53 bits.
//
// Stubbed (logged): MemAlloc (bump; some fail), MemFree, operator delete, LogReport / LogPanic, ASSERT_MSG (client.obj's),
// PTimeNow, TaskGetID / GetName / SetPriority / Sleep / ShouldISuspend / SuspendMe / ShouldIDie / Create / Suspend / Resume /
// IsSuspended / Destroy, MultiBegin / _MultiEnter / _MultiLeave / _MultiEnd, Win32Idle, Xlator::xlate, atexit, the gx screen
// calls and SplashGeneric, GetGameState, PhysTaskRestart, ConvertPTimeToPhysicsTime, PhysicsAbortRace / GetDeviation /
// IsPaused, CarMgrCount / GetInfo, GhostCarIncognito, LocalCar::FillNetPacket, NetCar::NewPacket, HackHornBall, OptionsGet,
// CarFileLoadSetup, GetTrackName, AIResetDriverMap, CreateTrackVersion, SessionDiscReasonString, the SessionMgr's members
// the code calls (Ok, ~, CanDestroySafely, Tick, GetServiceTable, ServiceRequestBroadcast, UnboundConnect x2, Disconnect,
// Shutdown, Send, SendReliable, Recv, GetStatus, GetDiscReason) and ReliableDataPort::GetEstRTLatency, the socket's LinkAlive,
// the deity's slots, the fake client's and server's. The game's own code everywhere else: PoolBase, Xlator's constructor,
// MakeNetPacket / MakePhysicsPacket, smart_strncpy, strstr / strchr / atoi, and every function of these two objects (each
// rewrite checked with the original callees).
//
// Built with /DVP_NET_FIXES, the rewrites have their fixes on (docs/PORTING.md, "Fixes"; docs/FIXES.md, "Multiplayer").
// Every function is still compared as above, with the rounds that reach a fixed case kept out of the comparison and
// counted -- get_userdata, dispatch_UserInfoPacket and dispatch_RemoveUserPacket with a user index past 7,
// dispatch_NetCarInfoPacket with a car index outside 0..7, dispatch_ChatPacket with a text that has no NUL in the packet,
// MultiAdjustPacketDelay with a sum past 32 bits, and every round where the original faulted and the rewrite didn't in
// dispatch_ChatPacket ("latency" with no space; the empty scrollback with the pool's allocation failing) -- the rewrite
// still run on each and required to return cleanly (or to fault just where the original does, in an original callee).
// Then directed_fix_tests: each fix's bad case run on the original (it must crash, hang -- a thread given 2 s --, overrun,
// or act on the malformed input) and on the rewrite (a clean return, nothing written outside what it may write, and what
// the fix promises), and the boundary case on both, compared bit for bit. Without it (VP_FAITHFUL) every rewrite must
// match its original bit for bit.
//
//   fix build: as above with /DVP_NET_FIXES (the same run line; VP_ONLY=fix: the directed tests alone)
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
#include <initializer_list>

#ifndef VP_NET_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals (docs/PORTING.md, "Fixes")
#define NET_FIXES 0
#else
#define NET_FIXES 1
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

// what net_wsock.h gives the rewrites: here N0's sites unpatched (the plain calls, as the originals make them)
int __cdecl net_PTimeNow() { return ((int(__cdecl*)())(uintptr_t)0x00413b40)(); }
int __cdecl net_Random(int r) { return ((int(__cdecl*)(int))(uintptr_t)0x0041b6e0)(r); }
void __fastcall net_Grab(void* lmi, void*) { ((void(__fastcall*)(void*, int))(uintptr_t)0x004a2d10)(lmi, 0); }
void __fastcall net_Release(void* lmi, void*) { ((void(__fastcall*)(void*, int))(uintptr_t)0x004a2e50)(lmi, 0); }
void __cdecl net_lobby_sleep(int ms) { ((void(__cdecl*)(int))(uintptr_t)0x00414de0)(ms); }
void __cdecl net_lobby_suspend_me() { ((void(__cdecl*)())(uintptr_t)0x00414df0)(); }

#include "../hook/net_multi.cpp"
#include "../hook/net_client.cpp"
static bool g_mark = (g_reg_leftover = true);
#include "../hook/net_leftover.cpp"

using namespace nr;
static uint8_t hg8(uint32_t a) { return *(volatile uint8_t*)(uintptr_t)a; }
static uint32_t hg32(uint32_t a) { return *(volatile uint32_t*)(uintptr_t)a; }
static void hs8(uint32_t a, uint8_t v) { *(volatile uint8_t*)(uintptr_t)a = v; }
static void hs32(uint32_t a, uint32_t v) { *(volatile uint32_t*)(uintptr_t)a = v; }

// ---- random numbers -----------------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static int irange(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

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
static void patch_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
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
#define U(p) ((uint32_t)(uintptr_t)(p))

// ---- the harness state and the fixed objects, in the arena (restored with it: both passes see the same) ---------------------
enum : uint32_t {
    A_STATE = 0x0, A_SM = 0x1000, A_SESSIONS = 0x1100, A_SOCK = 0x1300, A_SVC = 0x1400, A_RECVQ = 0x1800, A_DEITY = 0x2c00,
    A_FCL = 0x2d00, A_FSV = 0x2e00, A_LMI = 0x2f00, A_CARS = 0x3000, A_PROP = 0x3400, A_NAMES = 0x3600,
    A_SCRATCH = 0x4000, A_HEAP = 0x30000,
};
enum { RECVQ_MAX = 12 };
struct HState {
    uint32_t brk, alloc_calls, fail_mask;
    int32_t now, step;
    int32_t task_cur;
    uint32_t seed;                       // FillNetPacket's numbers
    uint32_t recv_n, recv_i;
    uint8_t status[8];                   // GetStatus's answers, in turn
    uint32_t status_i;
    int32_t disc_reason, uconnect, latency;
    uint8_t sm_ok, link_alive, paused, horn;
    uint32_t candestroy_after, candestroy_calls;
    int32_t game_state;
    uint32_t dev_bits;
    int32_t carmgr_count;
    uint8_t ghost, _g1[3];
    int32_t carinfo_type;
    uint32_t finished_mask;
    uint32_t suspended_after, suspended_calls;
    uint32_t should_suspend_mask, should_suspend_calls;
    uint32_t die_after, die_calls;
    int32_t task_create_id, multi_handle;
    uint32_t grab_fail_mask, grab_calls;
    int32_t cl_states[8];                // the fake client's states, one per Tick (then it stays)
    uint32_t cl_n, cl_i;
    int32_t gf_time;
    uint32_t es_after, es_calls;         // the fake server's EveryoneSynchronized: 1 from call es_after on
    uint8_t cl_human, _c1[3];
    int32_t carinfo_storage;
    uint8_t grab_never, no_proposal, _c2[2];  // (the fix tests: no screen to grab; the fake client has no proposal)
};
static HState* HS() { return (HState*)(g_arena + A_STATE); }
struct RecvPkt { int32_t len; uint8_t data[0xe4]; };
static RecvPkt* recvq(uint32_t i) { return (RecvPkt*)(g_arena + A_RECVQ + 0xf0 * i); }
static_assert(sizeof(RecvPkt) <= 0xf0 && A_RECVQ + 0xf0 * RECVQ_MAX <= A_DEITY, "recv queue");

// ---- the stubs' call log -------------------------------------------------------------------------------------------------------
enum { LOG_MAX = 16384 };
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
    __try { L(n); L(fnv(p, n)); } __except (EXCEPTION_EXECUTE_HANDLER) { L('BADP'); }
}
static void L_str(const char* s) {
    __try { const size_t n = strnlen(s, 4096); L((uint32_t)n); L(fnv(s, n)); } __except (EXCEPTION_EXECUTE_HANDLER) { L('BADS'); }
}
// a pointer the two passes share (the arena, the game, small values) by its value; a stack pointer only as "a stack pointer"
static void L_ptr(uint32_t w) {
    const uint32_t a = U(g_arena);
    if ((w >= a && w < a + ARENA_BYTES) || (w >= 0x400000 && w < 0x700000) || w < 0x10000 || (w >= 0x10000000 && w < 0x10200000)) L(w);
    else L('STK*');
}
static void L_fmt(const char* fmt, const uint32_t* a) {
    L((uint32_t)(uintptr_t)fmt);
    int k = 0;
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
}

// ---- the stubs --------------------------------------------------------------------------------------------------------------
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
static void __cdecl stub_MemFree(void* p) { L('MFRE'); L_ptr(U(p)); }
static void __cdecl stub_delete(void* p) { L('DEL '); L_ptr(U(p)); }
static int __cdecl stub_PTimeNow() { HState* s = HS(); s->now += s->step; L('PTIM'); return s->now; }
static int __cdecl stub_TaskGetID() { L('TKID'); return HS()->task_cur; }
static char g_task_names[4][16] = {"main", "lobby", "physics", "other"};
static char* __cdecl stub_TaskGetName(int id) { L('TKNM'); L((uint32_t)id); return g_task_names[(uint32_t)id & 3]; }
static void __cdecl stub_TaskSetPriority(int p) { L('TPRI'); L((uint32_t)p); }
static void __cdecl stub_TaskSleep(int ms) { L('TSLP'); L((uint32_t)ms); }
static uint8_t __cdecl stub_TaskShouldISuspend() { HState* s = HS(); L('TSIS'); return (uint8_t)(s->should_suspend_mask >> (s->should_suspend_calls++ & 31) & 1); }
static void __cdecl stub_TaskSuspendMe() { L('TSME'); }
static uint8_t __cdecl stub_TaskShouldIDie() { HState* s = HS(); L('TDIE'); return (uint8_t)(++s->die_calls >= s->die_after); }
static int __cdecl stub_TaskCreate(const char* name, void* fn) { L('TCRE'); L_str(name); L_ptr(U(fn)); return HS()->task_create_id; }
static void __cdecl stub_TaskSuspend(int id) { L('TSUS'); L((uint32_t)id); }
static void __cdecl stub_TaskResume(int id) { L('TRES'); L((uint32_t)id); }
static uint8_t __cdecl stub_TaskIsSuspended(int id) { HState* s = HS(); L('TISS'); L((uint32_t)id); return (uint8_t)(++s->suspended_calls >= s->suspended_after); }
static void __cdecl stub_TaskDestroy(int id) { L('TDST'); L((uint32_t)id); }
static int __cdecl stub_MultiBegin(const char* name) { L('MBEG'); L_str(name); return HS()->multi_handle; }
static void __cdecl stub_MultiEnter(int h, const char* f, int l) { L('MENT'); L((uint32_t)h); L(U(f)); L((uint32_t)l); }
static void __cdecl stub_MultiLeave(int h, const char* f, int l) { L('MLEV'); L((uint32_t)h); L(U(f)); L((uint32_t)l); }
static void __cdecl stub_MultiEnd(int h, const char* f, int l) { L('MEND'); L((uint32_t)h); L(U(f)); L((uint32_t)l); }
static void __cdecl stub_Win32Idle() { L('IDLE'); }
static void __fastcall stub_xlate(uint32_t* x, int) {
    L('XLAT'); L((uint32_t)(uintptr_t)x);
    x[1] = x[0];
    x[2] = *(uint32_t*)(uintptr_t)G_XLATOR_COOKIE;
}
static int __cdecl stub_atexit(void* f) { L('ATEX'); L((uint32_t)(uintptr_t)f); return 0; }
static uint8_t __cdecl stub_gxGrabScreen(void*) {
    HState* s = HS();
    L('GGRB');
    const uint32_t k = s->grab_calls++;
    if (s->grab_never) return 0;
    return (uint8_t)!(k < 32 && (s->grab_fail_mask >> k & 1));
}
static void* __cdecl stub_gxSetCanvas(void* c) { L('GCAN'); L_ptr(U(c)); return 0; }
static void __cdecl stub_gxClear(uint32_t c) { L('GCLR'); L(c); }
static void __cdecl stub_gxReleaseScreen() { L('GREL'); }
static void __cdecl stub_gxFlip() { L('GFLP'); }
static void __cdecl stub_SplashGeneric(const char* t) { L('SPLS'); L_str(t); }
static int __cdecl stub_GetGameState() { L('GSTA'); return HS()->game_state; }
static void __cdecl stub_PhysTaskRestart() { L('PRST'); }
static float __cdecl stub_PTimeToPhys(int t) { L('P2PH'); L((uint32_t)t); return (float)t * 0.001f; }
static void __cdecl stub_PhysicsAbortRace() { L('PABR'); }
static float __cdecl stub_PhysicsGetDeviation() { L('PDEV'); return bitsf(HS()->dev_bits); }
static uint8_t __cdecl stub_PhysicsIsPaused() { L('PPAU'); return HS()->paused; }
static int __cdecl stub_CarMgrCount() { L('CMCN'); return HS()->carmgr_count; }
static void* __cdecl stub_CarMgrGetInfo(int i) { L('CMGI'); L((uint32_t)i); HS()->carinfo_storage = HS()->carinfo_type; return &HS()->carinfo_storage; }
static uint8_t __cdecl stub_GhostCarIncognito() { L('GHST'); return HS()->ghost; }
static void __fastcall stub_FillNetPacket(void* car, int, uint8_t* phys) {
    HState* s = HS();
    L('FILL'); L_ptr(U(car));
    float* f = (float*)phys;
    for (int k = 0; k < 15; k++) {
        s->seed = s->seed * 1103515245u + 12345u;
        f[k] = (float)((int32_t)(s->seed >> 8) % 20000) * 0.37f;
    }
    *(int32_t*)(phys + 0x14) = (int32_t)(s->seed % 8) - 1;
    for (int k = 0; k < 4; k++) *(float*)(phys + 0x28 + 4 * k) = k == 3 ? 1.0f : 0.0f;
    phys[0x38] = (uint8_t)(s->seed & 1); phys[0x39] = (uint8_t)(s->seed >> 1 & 1);
    for (int k = 0; k < 4; k++) phys[0x3a + k] = (uint8_t)(s->seed >> (3 + k) & 1);
}
static void __fastcall stub_NewPacket(void* car, int, const uint8_t* phys) {
    L('NEWP'); L_ptr(U(car));
    uint8_t m[0x3e];
    memcpy(m, phys, 0x3e);
    memset(m, 0, 4);                                                    // MakePhysicsPacket never writes 0..3 and 0x10..0x13
    memset(m + 0x10, 0, 4);
    L_bytes(m, 0x3e);
}
static uint8_t __cdecl stub_HackHornBall() { L('HORN'); return HS()->horn; }
static void __cdecl stub_OptionsGet(const char* sec, const char* key, char* out, int n) {
    L('OPTG'); L_str(sec); L_str(key); L((uint32_t)n);
    strcpy(out, "Racer");
}
static uint8_t __cdecl stub_CarFileLoadSetup(uint8_t* setup, const char* car, const char* track) {
    L('CFLS'); L_str(car); L_str(track);
    for (int i = 0; i < 0x98; i++) setup[i] = (uint8_t)(i * 7 + (uint8_t)car[0]);
    return 1;
}
static char g_track[16] = "Bemidji";
static const char* __cdecl stub_GetTrackName(int t) { L('TRKN'); L((uint32_t)t); return g_track; }
static void __cdecl stub_AIResetDriverMap() { L('AIRS'); }
static void __cdecl stub_CreateTrackVersion(int32_t* v) { L('CTRV'); for (int i = 0; i < 8; i++) v[i] = 0x1000 + i * 0x111; }
static const char* __cdecl stub_SessionDiscReasonString(int r) { L('SDRS'); L((uint32_t)r); return r >= 0 && r < 7 ? (const char*)0x004fd630 : 0; }
static void __cdecl stub_ASSERT(int cond, const char* fmt, ...) {
    const uint32_t* a = (const uint32_t*)(&fmt + 1);
    L('ASRT'); L((uint32_t)cond); L(U(fmt));
    if (U(fmt) != S_INVALID_USER) { L(a[0]); L(a[1]); L_ptr(a[2]); }
}
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }

// the SessionMgr's members (received as __fastcall)
static uint8_t __fastcall sm_Ok(void*, int) { L('SMOK'); return HS()->sm_ok; }
static void __fastcall sm_dtor(void* m, int) { L('SMDT'); L_ptr(U(m)); }
static uint8_t __fastcall sm_CanDestroy(void*, int) { HState* s = HS(); L('SMCD'); return (uint8_t)(++s->candestroy_calls >= s->candestroy_after); }
static void __fastcall sm_Tick(void* m, int) { L('SMTK'); L_ptr(U(m)); }
static void* __fastcall sm_GetServiceTable(void*, int) { L('SMST'); return g_arena + A_SVC; }
static void __fastcall sm_SRB(void*, int, uint32_t type) { L('SMSR'); L(type); }
static int __fastcall sm_UConnect_id(void*, int, uint32_t id) { L('SMUI'); L(id); return HS()->uconnect; }
static int __fastcall sm_UConnect_rs(void*, int, const void* rs) { L('SMUR'); L_ptr(U(rs)); return HS()->uconnect; }
static void __fastcall sm_Disconnect(void*, int, int ch, int why) { L('SMDC'); L((uint32_t)ch); L((uint32_t)why); }
static void __fastcall sm_Shutdown(void*, int) { L('SMSD'); }
static void __fastcall sm_Send(void*, int, int ch, const uint8_t* pkt, int len, uint32_t fl) {
    L('SEND'); L((uint32_t)ch); L((uint32_t)len); L(fl & 0xff);
    uint8_t m[0x400];
    const int n = len > 2 && len < 0x400 ? len : 2;
    __try { memcpy(m, pkt, n); } __except (EXCEPTION_EXECUTE_HANDLER) { L('BADP'); return; }
    for (int r = 2; r + 24 <= n; r += 24) m[r + 23] &= 0x7f;           // MakeNetPacket never writes bit 7 of a car's last byte
    L_bytes(m + 2, (uint32_t)(n - 2));                                 // [0] / [1]: Send's own
}
// the bytes of a game packet its builder never writes (hook/net_wsock.cpp's net_send_mask, for this code's packets): frame
// garbage, which differs between the passes where a callee builds it (its frame sits elsewhere under the rewrite's)
static void mask_cstr(uint8_t* m, int n, int at, int len) {
    for (int i = at; i < at + len && i < n; i++)
        if (!m[i]) { for (int j = i + 1; j < at + len && j < n; j++) m[j] = 0; return; }
}
static void mask_range(uint8_t* m, int n, int from, int to) { for (int i = from; i <= to && i < n; i++) m[i] = 0; }
static void mask_game_packet(uint8_t* p, int n) {
    if (n < 4) return;
    switch (p[3]) {
    case 0x21: mask_cstr(p, n, 4, 13); break;
    case 0x28: mask_cstr(p, n, 4, 50); break;
    case 0x29: mask_cstr(p, n, 8, 50); break;
    case 0x30: {
        uint32_t user = 0;
        if (n >= 9) memcpy(&user, p + 5, 4);
        mask_range(p, n, 4, 4);
        if (!user) mask_range(p, n, 9, n - 1);
        else { mask_range(p, n, 9, 12); mask_range(p, n, 15, 25); mask_cstr(p, n, 26, 32); mask_range(p, n, 58, 60); mask_range(p, n, 206, 221); }
        break;
    }
    case 0x37: case 0x38: mask_range(p, n, 8, 51); break;
    case 0x3b: case 0x3c: mask_range(p, n, 15, 15); break;
    }
}
static void __fastcall sm_SendReliable(void*, int, int ch, const uint8_t* pkt, int len, const void* rpi, uint32_t fl) {
    L('SREL'); L((uint32_t)ch); L((uint32_t)len); L_ptr(U(rpi)); L(fl & 0xff);
    uint8_t m[0x400];
    const int n = len > 3 && len < 0x400 ? len : 3;
    __try { memcpy(m, pkt, n); } __except (EXCEPTION_EXECUTE_HANDLER) { L('BADP'); return; }
    mask_game_packet(m, n);
    L_bytes(m + 3, (uint32_t)(n - 3));                                 // [0..2]: SendReliable's / HostEntry's
}
static uint8_t __fastcall sm_Recv(void*, int, int ch, uint8_t* buf, int32_t* len) {
    HState* s = HS();
    L('RECV'); L((uint32_t)ch); L((uint32_t)*len);
    if (s->recv_i >= s->recv_n) return 0;
    RecvPkt* p = recvq(s->recv_i++);
    memcpy(buf, p->data, 0xe2);
    *len = p->len;
    return 1;
}
static uint8_t __fastcall sm_GetStatus(void*, int, int ch) { HState* s = HS(); L('SMGS'); L((uint32_t)ch); return s->status[s->status_i++ & 7]; }
static int __fastcall sm_GetDiscReason(void*, int, int ch) { L('SMDR'); L((uint32_t)ch); return HS()->disc_reason; }
static int __fastcall rdp_Latency(void* rdp, int, const void* addr) { L('LATN'); L_ptr(U(rdp)); L_ptr(U(addr)); return HS()->latency; }
static uint8_t __fastcall so_LinkAlive(void*, int) { L('SLNK'); return HS()->link_alive; }
static void* g_sock_vt[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, (void*)so_LinkAlive, 0, 0, 0, 0, 0};

// the deity's slots
static void __fastcall dt_StartRaceAt(void*, int, uint32_t f) { L('DSTA'); L(f); }
static void __fastcall dt_Update(void*, int) { L('DUPD'); }
static uint8_t __fastcall dt_CarIsFinished(void*, int, int i) { L('DFIN'); L((uint32_t)i); return (uint8_t)(HS()->finished_mask >> (i & 31) & 1); }
static void __fastcall dt_NetCast(void*, int, const uint8_t* p) { L('DCST'); L_bytes(p, 12); }
static void __fastcall dt_none(void*, int) { L('DNON'); }
static void* g_deity_vt[32];

// the fake IRaceClient (multi.obj's tests): its state at +4
static int32_t* fcl_state(void* c) { return (int32_t*)((uint8_t*)c + 4); }
static void* __fastcall fcl_dtor(void* c, int, uint32_t fl) { L('CDTR'); L(fl); return c; }
static void __fastcall fcl_Tick(void* c, int) {
    HState* s = HS();
    L('CTIK');
    if (s->cl_i < s->cl_n) *fcl_state(c) = s->cl_states[s->cl_i++];
}
static void __fastcall fcl_SetOwner(void*, int, int id) { L('CSOW'); L((uint32_t)id); }
static void __fastcall fcl_Disconnect(void*, int) { L('CDIS'); }
static void __fastcall fcl_Speak(void*, int, const char* t) { L('CSPK'); L_str(t); }
static void __fastcall fcl_Synchronize(void*, int) { L('CSYN'); }
static void __fastcall fcl_WorldLoaded(void*, int) { L('CWLD'); }
static void __fastcall fcl_GetGreenflagTime(void*, int, int32_t* t) { L('CGFT'); *t = HS()->gf_time; }
static const void* __fastcall fcl_GetRaceInfo(void*, int) { L('CGRI'); return g_arena + A_PROP + 0x40; }
static const void* __fastcall fcl_GetProposal(void*, int) { L('CGPR'); return HS()->no_proposal ? 0 : g_arena + A_PROP; }
static void __fastcall fcl_GetCarList(void*, int, void* l) { L('CGCL'); L_ptr(U(l)); }
static void __fastcall fcl_Register(void*, int, void* car, int i) { L('CREG'); L_ptr(U(car)); L((uint32_t)i); }
static void __fastcall fcl_UnRegister(void*, int, void* car) { L('CURG'); L_ptr(U(car)); }
static void __fastcall fcl_SendAll(void*, int) { L('CSND'); }
static uint8_t __fastcall fcl_CarIsHuman(void*, int, int i) { L('CHUM'); L((uint32_t)i); return HS()->cl_human; }
static void __fastcall fcl_DeityCast(void*, int, const void* p, int n) { L('CDCS'); L_ptr(U(p)); L((uint32_t)n); }
static void __fastcall fcl_none(void*, int) { L('CNON'); }
static void* g_fcl_vt[40];
// the fake IRaceServer
static void* __fastcall fsv_dtor(void* c, int, uint32_t fl) { L('VDTR'); L(fl); return c; }
static void __fastcall fsv_SetOwner(void*, int, int id) { L('VSOW'); L((uint32_t)id); }
static void __fastcall fsv_Tick(void*, int) { L('VTIK'); }
static void __fastcall fsv_SendAll(void*, int) { L('VSND'); }
static uint8_t __fastcall fsv_EveryoneSync(void*, int) { HState* s = HS(); L('VESY'); return (uint8_t)(++s->es_calls >= s->es_after); }
static void __fastcall fsv_Restart(void*, int) { L('VRST'); }
static void __fastcall fsv_BackToChat(void*, int) { L('VB2C'); }
static void __fastcall fsv_none(void*, int) { L('VNON'); }
static void* g_fsv_vt[16];

// ---- the raw call -------------------------------------------------------------------------------------------------------------
static uint32_t g_c_fn, g_c_ecx, g_c_edx, g_c_n, g_c_args[16];
static uint32_t g_r_eax, g_r_edx, g_r_ebx, g_r_esi, g_r_edi, g_r_ebp, g_r_esp, g_c_top, g_save_esp;
static uint16_t g_r_sw;
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

// ---- the world -------------------------------------------------------------------------------------------------------------------
static uint32_t g_scratch_at;
static uint8_t* sv(uint32_t n) {                  // a scratch buffer, random-filled
    g_scratch_at = (g_scratch_at + 15) & ~15u;
    uint8_t* p = g_arena + A_SCRATCH + g_scratch_at;
    g_scratch_at += n;
    if (A_SCRATCH + g_scratch_at > A_HEAP - 0x8000) { printf("scratch overflow\n"); ExitProcess(4); }
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)rnd();
    return p;
}
static RaceClient* g_rc;
static netc::SessionMgr* g_sm;
static uint8_t* car_obj(int i) { return g_arena + A_CARS + 0x40 * (uint32_t)i; }   // 0..7 LocalCars, 8..15 NetCars
static const char* const k_names[] = {"Alpha", "bravo", "", "Charlie_long1", "d", "Echo", "host1", "host2"};

static int32_t user_id(int slot) { return (int32_t)(((uint32_t)irange(1, 0x3f) << 8) | (uint32_t)slot); }
static int32_t some_user() {
    if (chance(80)) {
        for (int t = 0; t < 8; t++) { const int k = irange(0, 7); if (g_rc->users[k].id) return g_rc->users[k].id; }
    }
    return chance(50) ? user_id(irange(0, 7)) : (chance(50) ? 0 : (int32_t)rnd());
}

// a game packet the server would send (session data class 1), of type t (or random); returns its length
static int make_game_packet(uint8_t* p, int t) {
    for (int i = 0; i < 0xe4; i++) p[i] = (uint8_t)rnd();
    p[0] = (uint8_t)(((rnd() % 8) << 4) | (chance(92) ? 1 : rnd() & 0xf));
    p[3] = (uint8_t)t;
    switch (t) {
    case 0x23: {                                                       // UserList
        int32_t me = chance(80) ? user_id(irange(0, 7)) : some_user();
        memcpy(p + 4, &me, 4);
        for (int i = 0; i < 8; i++) {
            int32_t id = chance(30) ? 0 : chance(60) && g_rc->users[i].id ? g_rc->users[i].id : user_id(i);
            memcpy(p + 8 + 4 * i, &id, 4);
        }
        return 0x28;
    }
    case 0x25: {                                                       // UserInfo
        int32_t id = chance(90) ? user_id(irange(0, 7)) : (int32_t)(rnd() & 0x3f3f);
        memcpy(p + 4, &id, 4);
        const char* nm = k_names[rnd() % 8];
        memset(p + 8, 0, 13);
        memcpy(p + 8, nm, strlen(nm) < 12 ? strlen(nm) : 12);
        if (chance(10)) memset(p + 8, 'x', 13);
        int32_t st = irange(0, 3);
        memcpy(p + 0x15, &st, 4);
        return 0x19;
    }
    case 0x26: { int32_t id = chance(90) ? some_user() : (int32_t)(rnd() & 0x3f3f); memcpy(p + 4, &id, 4); return 8; }
    case 0x27: {                                                       // Chat
        int32_t id = some_user();
        memcpy(p + 4, &id, 4);
        p[8] = (uint8_t)(rnd() & 1);
        const char* texts[] = {"hello there", "latency 120", "latency 5000", "latency -3", "gg", "x latency 77 y", "latency  9"};
        const char* tx = texts[rnd() % 7];
        if (chance(3)) tx = "latency";                                  // no space: atoi(strchr(...) == 0) faults in both
        strcpy((char*)p + 9, tx);
        if (chance(10)) memset(p + 9, 'z', 0x40);
        return (int)(9 + strlen(tx) + 1);
    }
    case 0x2d: for (int i = 0; i < 8; i++) p[4 + i] = (uint8_t)irange(0, 3); return 0xc;
    case 0x2e: { int32_t s = chance(85) ? (int32_t)(rnd() & 1 ? 9 : rnd() & 1 ? 0xf : rnd() & 1 ? 0x11 : 0x12) : irange(0, 0x20); memcpy(p + 4, &s, 4); return 8; }
    case 0x31:                                                         // CommenceCarGet
        for (int i = 0; i < 8; i++) { int32_t id = chance(40) ? 0 : some_user(); memcpy(p + 4 + 4 * i, &id, 4); }
        return 0x24;
    case 0x33: {                                                       // NetCarInfo
        int i = chance(90) ? irange(0, 7) : irange(-3, 10);
        p[4] = (uint8_t)(int8_t)i;
        int32_t u = (i >= 0 && i < 8 && chance(75)) ? g_rc->car_req[i] : some_user();
        memcpy(p + 5, &u, 4);
        return 0xde;
    }
    case 0x37: { int32_t t0 = HS()->now - irange(0, 500); memcpy(p + 4, &t0, 4); return 0x34; }
    case 0x3a: { int32_t t0 = HS()->now + irange(0, 5000); memcpy(p + 4, &t0, 4); return 8; }
    default: return irange(4, 0x40);
    }
}
static const int k_types[] = {0x23, 0x25, 0x26, 0x27, 0x2a, 0x2c, 0x2d, 0x2e, 0x2f, 0x31, 0x33, 0x37, 0x3a, 0x3c, 0x24, 0x30, 0x55};
static int make_race_packet(uint8_t* p) {
    for (int i = 0; i < 0xe4; i++) p[i] = (uint8_t)rnd();
    p[0] = (uint8_t)(((rnd() % 8) << 4) | 2);
    const int k = irange(0, 8);
    for (int c = 0; c < k; c++) {
        uint8_t* r = p + 2 + 24 * c;
        r[0] = (uint8_t)(chance(90) ? irange(0, 7) : irange(-4, 12));
    }
    return 2 + 24 * k + (chance(10) ? irange(0, 23) : 0);
}

static void set_states(int st0) {
    HState* s = HS();
    s->cl_n = (uint32_t)irange(0, 6);
    for (uint32_t i = 0; i < s->cl_n; i++) s->cl_states[i] = irange(0, 3) == 0 ? (rnd() & 1 ? 0xf : 0x11) : irange(5, 0x11);
    // the last one ends every wait (MultiSynchronize's): out of 5..0x11
    static const int32_t ends[] = {2, 3, 4, 0x12, 0x14};
    s->cl_states[s->cl_n] = ends[rnd() % 5];
    s->cl_n++;
    (void)st0;
}

static void build_world(bool multi) {
    HState* s = HS();
    memset(s, 0, sizeof *s);
    s->brk = A_HEAP;
    s->now = (int32_t)(rnd() % 100000) + 50000;
    s->step = chance(15) ? 0 : irange(1, 600);
    s->task_cur = irange(1, 4);
    s->seed = rnd();
    g_scratch_at = 0;
    // the fake SessionMgr (only the fields this code reads directly: sessions, nremote, sock, task, rdp's address)
    g_sm = (netc::SessionMgr*)(g_arena + A_SM);
    memset(g_sm, 0, sizeof *g_sm);
    g_sm->sessions = (netc::SessionInfo*)(g_arena + A_SESSIONS);
    for (int i = 0; i < 8 * 0x1c; i++) ((uint8_t*)g_sm->sessions)[i] = (uint8_t)rnd();
    g_sm->nsessions = 8;
    g_sm->nremote = irange(0, 6);
    g_sm->sock = g_arena + A_SOCK;
    *(void***)(g_arena + A_SOCK) = g_sock_vt;
    g_sm->task = s->task_cur;
    for (int i = 0; i < 8; i++) {
        uint8_t* e = g_arena + A_SVC + 0x54 * i;
        for (int k = 0; k < 0x54; k++) e[k] = (uint8_t)rnd();
        strcpy((char*)e, chance(70) ? k_names[6 + (i & 1)] : k_names[rnd() % 8]);
        const uint32_t id = 1000 + (uint32_t)irange(0, 5);
        memcpy(e + 0x28, &id, 4);
    }
    for (int i = 0; i < 8; i++) strcpy((char*)g_arena + A_NAMES + 16 * i, k_names[i]);
    // the deity
    *(void***)(g_arena + A_DEITY) = g_deity_vt;
    hs32(G_DEITY, U(g_arena + A_DEITY));
    // the harness's answers
    for (int i = 0; i < 8; i++) {
        static const uint8_t st[] = {0, 1, 0x80, 0x81, 0x82, 0x84, 0x86, 0x83, 0x11, 0x85};
        s->status[i] = st[rnd() % 10];
    }
    s->disc_reason = irange(0, 8);
    s->uconnect = chance(30) ? -1 : irange(0, 7);
    s->latency = irange(0, 900);
    s->sm_ok = (uint8_t)(chance(85) ? 1 : 0);
    s->link_alive = (uint8_t)(chance(85) ? 1 : 0);
    s->paused = (uint8_t)(chance(15) ? 1 : 0);
    s->horn = (uint8_t)(chance(30) ? 1 : 0);
    s->candestroy_after = (uint32_t)irange(1, 4);
    s->game_state = irange(0, 3);
    switch (rnd() % 6) {
    case 0: s->dev_bits = 0x7fc00000u | (rnd() & 0x3fffff); break;
    case 1: s->dev_bits = 0x3dcccccdu; break;                           // 0.1f
    case 2: s->dev_bits = fbits(0.5f); break;
    default: s->dev_bits = fbits((float)(rnd() % 1000) * 0.0001f); break;
    }
    s->carmgr_count = irange(0, 8);
    s->ghost = (uint8_t)(rnd() & 1);
    s->carinfo_type = irange(0, 4);
    s->finished_mask = chance(50) ? 0xffffffffu : rnd();
    s->suspended_after = (uint32_t)irange(1, 5);
    s->should_suspend_mask = rnd() & rnd();
    s->die_after = (uint32_t)irange(1, 4);
    s->task_create_id = irange(1, 8);
    s->multi_handle = 0x77;
    s->grab_fail_mask = chance(50) ? 0 : rnd() & rnd() & 0xff;
    s->gf_time = irange(-2000, 9000);
    s->es_after = (uint32_t)irange(1, 4);
    s->cl_human = (uint8_t)(rnd() & 1);
    set_states(0);
    // the statics both objects read
    hs8(G_WORLD_LIVE, (uint8_t)(chance(75) ? 1 : 0));
    hs32(G_PACKET_DELAY, (uint32_t)irange(0, 400));
    hs8(G_MONITOR, (uint8_t)(rnd() & 1));
    hs32(G_TROUBLE_T, (uint32_t)(s->now - irange(-100, 600)));
    hs8(G_RESTART, (uint8_t)(chance(30) ? 1 : 0));
    hs8(G_PRE_RACE, (uint8_t)(chance(50) ? 1 : 0));
    hs8(G_ESCAPE, (uint8_t)(chance(25) ? 1 : 0));
    hs8(G_TROUBLE, (uint8_t)(chance(40) ? 1 : 0));
    hs32(G_AUTOEND, chance(50) ? 0 : (uint32_t)(s->now + irange(-3000, 3000)));
    // the function-local Xlators: built (by a first call) or not, translated or stale
    if (chance(60)) call_orig(0x004a7bf0, 0, {3});
    if (chance(60)) call_orig(0x004aa330, 0, {0x101});
    if (chance(30)) hs8(G_SYNC_GUARD, (uint8_t)(rnd() & 3));
    if (chance(30)) hs32(G_XLATOR_COOKIE, rnd());
    // multi.obj: the fake client and server and the LiveMultiInfo
    uint8_t* fcl = g_arena + A_FCL;
    *(void***)fcl = g_fcl_vt;
    *fcl_state(fcl) = chance(50) ? irange(0, 0x16) : (rnd() & 1 ? 0xf : irange(4, 0x12));
    uint8_t* fsv = g_arena + A_FSV;
    *(void***)fsv = g_fsv_vt;
    for (int i = 0; i < 0x80; i++) g_arena[A_PROP + i] = (uint8_t)rnd();
    *(int32_t*)(g_arena + A_PROP + 0x30) = irange(0, 9);
    LiveMultiInfo* m = (LiveMultiInfo*)(g_arena + A_LMI);
    for (int i = 0; i < 0x3c; i++) ((uint8_t*)m)[i] = (uint8_t)rnd();
    m->live = (uint8_t)(chance(70) ? 1 : 0);
    m->server = chance(60) ? fsv : 0;
    m->client = fcl;
    m->sm = g_sm;
    m->task = chance(90) ? 5 : 0;
    m->owner = chance(50) ? s->task_cur : chance(50) ? 5 : 7;
    m->multi = 0x77;
    hs32(G_LMI, multi ? (chance(85) ? U(m) : 0) : 0);
    hs32(G_LMI_THREAD, U(m));
    // client.obj: a RaceClient by the original CreateRaceClient, then made random
    g_rc = 0;
    for (int t = 0; t < 4 && !g_rc; t++) {
        const uint32_t sm_ok = s->sm_ok;
        s->sm_ok = 1;
        switch (rnd() % 3) {
        case 0: g_rc = (RaceClient*)(uintptr_t)call_orig(0x004aa240, 0, {U(g_sm), U(g_arena + A_SVC)}); break;
        case 1: g_rc = (RaceClient*)(uintptr_t)call_orig(0x004aa290, 0, {U(g_sm), U(g_arena + A_NAMES + 16 * 6)}); break;
        default: g_rc = (RaceClient*)(uintptr_t)call_orig(0x004aa2e0, 0, {U(g_sm), 1002}); break;
        }
        s->sm_ok = (uint8_t)sm_ok;
        if (!g_rc) { s->uconnect = irange(0, 7); s->sm_ok = 1; }
    }
    if (!g_rc) { printf("setup: CreateRaceClient failed\n"); ExitProcess(4); }
    RaceClient* c = g_rc;
    // the client's Xlators: some not built yet (the constructors' Ok built them)
    if (chance(25)) hs8(G_STATUS_GUARD, (uint8_t)(rnd() & 0x1f));
    if (chance(25)) hs8(G_DISC_GUARD, (uint8_t)(rnd() & 0xf));
    c->state = chance(30) ? irange(0, 0x16) : irange(5, 0x16);
    c->reason = irange(0, 0x104);
    c->ok = (uint8_t)(chance(85) ? 1 : 0);
    c->chan = chance(10) ? -1 : irange(0, 7);
    for (int i = 0; i < 8; i++) {
        ClientData* u = &c->users[i];
        u->id = chance(25) ? 0 : user_id(i);
        memset(u->name, 0, 13);
        strcpy(u->name, k_names[rnd() % 6]);
        if (chance(5)) memset(u->name, 'n', 13);                        // no NUL: MaxNameLength reads on
        u->status = irange(0, 3);
        c->user_ids[i] = chance(70) ? u->id : chance(50) ? 0 : user_id(i);
    }
    c->me = chance(80) ? (c->users[irange(0, 7)].id ? c->users[irange(0, 7)].id : user_id(irange(0, 7))) : (int32_t)(rnd() & 0x3f07);
    for (int i = 0; i < 8; i++) {
        CarSlot* k = &c->cars[i];
        for (int b = 0; b < (int)sizeof *k; b++) ((uint8_t*)k)[b] = (uint8_t)rnd();
        k->user = chance(35) ? 0 : chance(70) ? c->users[i].id : some_user();
        k->localcar = chance(40) ? car_obj(i) : 0;
        k->netcar = chance(40) ? car_obj(8 + i) : 0;
        k->human = (uint8_t)(chance(50) ? 0 : rnd());
        c->car_req[i] = chance(60) ? k->user : chance(50) ? 0 : some_user();
    }
    for (int i = 0; i < 0x3c; i++) c->proposal[i] = (uint8_t)rnd();
    for (int i = 0; i < 0x34; i++) c->race_info[i] = (uint8_t)rnd();
    c->has_proposal = (uint8_t)(rnd() & 1);
    c->has_race_info = (uint8_t)(rnd() & 1);
    c->iroc_car = irange(-1, 9);
    c->next_search = s->now + irange(-5000, 5000);
    c->search_by_id = (uint8_t)(chance(30) ? 1 : 0);
    c->search_name = c->search_by_id ? (const char*)(uintptr_t)(1000 + irange(0, 6)) : (const char*)(g_arena + A_NAMES + 16 * irange(0, 7));
    c->greenflag = s->now + irange(-3000, 9000);
    c->user_changes = (uint8_t)(rnd() & 1);
    c->last_send = s->now - irange(0, 200);
    c->last_ping = s->now - irange(0, 20000);
    c->last_tick = chance(20) ? 0 : s->now - irange(0, 1500);
    c->last_race_pkt = s->now - irange(0, 20000);
    c->task = chance(85) ? s->task_cur : irange(1, 6);
    // a chat ring, grown by the original
    const int nchat = chance(30) ? 0 : chance(80) ? irange(1, 8) : irange(38, 46);
    for (int i = 0; i < nchat; i++) {
        uint8_t* p = sv(0xe4);
        make_game_packet(p, 0x27);
        int32_t id = c->users[i & 7].id ? c->users[i & 7].id : 0;
        if (!id) continue;
        memcpy(p + 4, &id, 4);
        strcpy((char*)p + 9, "chat line");
        call_orig(0x004a99e0, 1, {U(c), 0, U(p)});
    }
    // the packets Recv hands out
    s->recv_n = (uint32_t)irange(0, 6);
    for (uint32_t i = 0; i < s->recv_n; i++) {
        RecvPkt* p = recvq(i);
        if (chance(20)) p->len = make_race_packet(p->data);
        else p->len = make_game_packet(p->data, chance(90) ? k_types[rnd() % 14] : k_types[rnd() % 17]);
    }
    // the clock moves on; some of the test's allocations fail
    s->now += irange(0, 4000);
    s->alloc_calls = 0;
    s->fail_mask = chance(20) ? rnd() & rnd() : 0;
    // the waits must end: a clock that stands still only with the lobby task suspended at once
    if (s->step == 0) { s->suspended_after = 1; s->candestroy_after = 1; }
}

// ---- arguments per function -------------------------------------------------------------------------------------------------------
static bool args_for(const Ent& f, uint32_t* w) {
    for (int i = 0; i < 18; i++) w[i] = rnd();
    uint32_t* a = f.fast ? w + 2 : w;
    RaceClient* c = g_rc;
    LiveMultiInfo* m = (LiveMultiInfo*)(g_arena + A_LMI);
    if (f.fast) { w[0] = U(c); w[1] = rnd(); }
    switch (f.v10) {
    // ---- multi.obj ---------------------------------------------------------------------------------------------------------------
    case 0x004a2370: a[0] = U(m); return true;                         // MultiLiveBegin
    case 0x004a23d0: if (!hg32(G_LMI)) hs32(G_LMI, U(m)); return true;  // MultiIsServer (needs a live game)
    case 0x004a2700: a[0] = U(g_arena + A_NAMES); return true;         // clear_screen
    case 0x004a2760: case 0x004a27a0: a[0] = U(car_obj(irange(0, 15))); a[1] = (uint32_t)irange(0, 7); return true;
    case 0x004a27e0: case 0x004a2800: a[0] = U(car_obj(irange(0, 15))); return true;
    case 0x004a2850: {                                                 // MultiSynchronize(lmi, carlist, track)
        a[0] = U(m); a[1] = U(sv(0xc84)); a[2] = U(sv(0x40));
        // the client's state starts in the wait (mostly) and its Tick moves it on to a state that ends it
        *fcl_state(m->client) = chance(80) ? irange(5, 0x11) : irange(0, 0x14);
        return true;
    }
    case 0x004a2a90: a[0] = U(g_arena + A_NAMES + 16); return true;
    case 0x004a2ab0: a[0] = (uint32_t)(chance(80) ? irange(-600, 2400) : (int)rnd()); return true;
    case 0x004a2b10: a[0] = rnd(); return true;
    case 0x004a2b20: case 0x004a2b70: case 0x004a2a80: case 0x004a2820:
        if (!hg32(G_LMI)) hs32(G_LMI, U(m));
        if (f.v10 == 0x004a2b70) { a[0] = U(sv(12)); a[1] = 12; }
        return true;
    case 0x004a2b30: a[0] = (uint32_t)irange(-1, 9); return true;
    case 0x004a2be0: w[0] = U(sv(0x3c)); return true;                  // LiveMultiInfo::LiveMultiInfo
    case 0x004a2c40: case 0x004a2d10: case 0x004a2dc0: case 0x004a2e10: case 0x004a2e50: case 0x004a2ec0: case 0x004a2ed0:
    case 0x004a2f10:
        w[0] = U(m);
        if (f.v10 == 0x004a2c40 && chance(20)) m->sm = 0;
        return true;
    case 0x004a2e90: w[0] = U(m); a[0] = (uint32_t)irange(1, 8); return true;
    default: break;
    }
    if (f.v10 < 0x004a7000) return true;                               // the rest of multi.obj: no arguments
    // ---- client.obj ---------------------------------------------------------------------------------------------------------------
    switch (f.v10) {
    case 0x004a7bf0: a[0] = (uint32_t)irange(-2, 0x17); return true;
    case 0x004a8350: a[0] = (uint32_t)irange(-2, 4); return true;
    case 0x004aa330: a[0] = (uint32_t)(chance(60) ? irange(0xfe, 0x105) : irange(-2, 9)); return true;
    case 0x004aa4f0: a[0] = (uint32_t)irange(0x20, 0x3e); return true;
    case 0x004a7e70: case 0x004a7ed0: case 0x004a7f50: {               // the constructors (this: fresh memory)
        w[0] = U(sv(0x8cc));
        a[0] = U(g_sm);
        a[1] = f.v10 == 0x004a7e70 ? U(g_arena + A_SVC + 0x54 * irange(0, 5)) : f.v10 == 0x004a7ed0 ? U(g_arena + A_NAMES + 16 * irange(0, 7)) : 1000 + rnd() % 6;
        if (chance(10)) a[0] = 0;
        return true;
    }
    case 0x004aa240: case 0x004aa290: case 0x004aa2e0:
        a[0] = U(g_sm);
        a[1] = f.v10 == 0x004aa240 ? U(g_arena + A_SVC) : f.v10 == 0x004aa290 ? U(g_arena + A_NAMES + 16 * irange(0, 7)) : 1000 + rnd() % 6;
        return true;
    case 0x004a80a0: a[0] = U(g_arena + A_SVC + 0x54 * irange(0, 5)); return true;
    case 0x004a83f0: a[0] = 1000 + rnd() % 6; return true;
    case 0x004a80e0: a[0] = (uint32_t)irange(0, 7); return true;       // CarIsHuman
    case 0x004a8190: a[0] = U(sv(0x98)); a[1] = (uint32_t)irange(0, 7); return true;
    case 0x004a8210: a[0] = U(sv(0xc84)); return true;
    case 0x004a8670: case 0x004a8710: case 0x004a8780:
        a[0] = (uint32_t)(chance(70) ? some_user() : chance(50) ? user_id(irange(0, 7)) : (int32_t)(rnd() & 0x1f1f));
        return true;
    case 0x004a87f0: a[0] = U(chance(80) ? (uint8_t*)g_arena + A_NAMES + 16 * irange(0, 7) : sv(0x80)); return true;
    case 0x004a8880: a[0] = U(g_arena + A_NAMES + 16 * irange(0, 7)); a[1] = (uint32_t)some_user(); return true;
    case 0x004a89f0: a[0] = U(g_arena + A_NAMES + 16 * irange(0, 7)); a[1] = rnd(); return true;
    case 0x004a8b90: case 0x004a8c00: {
        int32_t* it = (int32_t*)sv(4);
        *it = irange(-1, 9);
        a[0] = U(it);
        return true;
    }
    case 0x004a8ca0: a[0] = (uint32_t)irange(-1, 9); return true;
    case 0x004a8e80: a[0] = U(sv(0x40)); a[1] = (uint32_t)irange(0, 0x20); return true;
    case 0x004a91a0: case 0x004a9280: {
        const int i = irange(0, 7);
        if (chance(50)) c->car_req[i] = c->cars[i].user;
        a[0] = U(car_obj(irange(0, 15))); a[1] = (uint32_t)i;
        return true;
    }
    case 0x004a9380: a[0] = U(sv(4)); if (chance(70)) c->state = 0x14; return true;
    case 0x004a94f0: a[0] = U(chance(80) ? car_obj(irange(0, 15)) : sv(4)); return true;
    case 0x004a97f0: case 0x004a9850: case 0x004a98a0: case 0x004a98c0: case 0x004a9930: case 0x004a99e0: case 0x004a9b10:
    case 0x004a9b50: case 0x004a9bd0: case 0x004a9c30: case 0x004a9c50: case 0x004a9cc0: case 0x004a9cf0: case 0x004a9d60: {
        uint8_t* p = sv(0xe4);
        int t = 0;
        switch (f.v10) {
        case 0x004a97f0: t = 0x37; if (chance(70)) c->state = 0x10; break;
        case 0x004a9850: t = 0x3a; break;
        case 0x004a98a0: t = 0x3c; break;
        case 0x004a98c0: t = 0x2d; break;
        case 0x004a9930: t = 0x33; break;
        case 0x004a99e0: t = 0x27; break;
        case 0x004a9b10: t = 0x23; break;
        case 0x004a9b50: t = 0x25; break;
        case 0x004a9bd0: t = 0x26; break;
        case 0x004a9c30: t = 0x2f; break;
        case 0x004a9c50: t = 0x2a; break;
        case 0x004a9cc0: t = 0x31; break;
        case 0x004a9cf0: t = 0x2c; break;
        default: t = 0x2e; break;
        }
        make_game_packet(p, t);
        a[0] = U(p);
        return true;
    }
    case 0x004a9de0: { uint8_t* p = sv(0xe4); const int len = make_race_packet(p); a[0] = U(p); a[1] = (uint32_t)len; return true; }
    case 0x004aa060: a[0] = U(sv(0x34)); return true;
    case 0x004aa100: a[0] = U(g_arena + A_NAMES + 16 * irange(0, 7)); a[1] = rnd(); return true;
    case 0x004aa220: a[0] = U(g_arena + A_NAMES + 16 * irange(0, 7)); return true;
    case 0x004aa620: a[0] = (uint32_t)irange(1, 6); return true;
    case 0x004a8f80: if (chance(70)) c->state = 0xd; return true;
    default: return true;                                              // the client alone
    }
}

// ---- the fix build: rounds kept out of the comparison ------------------------------------------------------------------------
// (the world and the arguments made, before the original runs) the rounds whose input reaches a fixed case
static bool fx_pre_case(const Ent& f, const uint32_t* w) {
    const uint32_t* a = f.fast ? w + 2 : w;
    switch (f.v10) {
    case 0x004a8670: return (a[0] & 0xff) >= 8;                                          // get_userdata: a user index past 7
    case 0x004a9b50: case 0x004a9bd0: {                                                  // UserInfo / RemoveUser: likewise
        uint32_t id;
        memcpy(&id, (const uint8_t*)(uintptr_t)a[0] + 4, 4);
        return (id & 0xff) >= 8;
    }
    case 0x004a9930: return (uint32_t)(int32_t)(int8_t)((const uint8_t*)(uintptr_t)a[0])[4] >= 8u;   // NetCarInfo: the car index
    case 0x004a99e0: return !memchr((const uint8_t*)(uintptr_t)a[0] + 9, 0, 0xe4 - 9);   // Chat: no NUL in the packet
    case 0x004a2ab0: {                                                                   // MultiAdjustPacketDelay: past 32 bits
        const int64_t sum = (int64_t)(int32_t)hg32(G_PACKET_DELAY) + (int32_t)a[0];
        return sum > INT32_MAX || sum < INT32_MIN;
    }
    default: return false;
    }
}
// (after both ran) a round where only the original faulted, in a function whose fix is for that fault
static bool fx_post_case(const Ent& f, const Result& ro, const Result& rn) {
    return f.v10 == 0x004a99e0 && ro.fault && !rn.fault;                                 // Chat: atoi(0), the empty ring
}

#if NET_FIXES
// ---- the fixes, each on its bad case (directed_fix_tests) -----------------------------------------------------------------
static int g_fx_bad, g_fx_n, g_fx_same_n;
static const Ent& fx_fn(uint32_t v10) {
    for (int i = 0; i < g_nfns; i++)
        if (g_fns[i].v10 == v10 && !g_fns[i].leftover) return g_fns[i];
    printf("  fix test: %08x isn't listed\n", v10);
    ExitProcess(4);
}
static void fx_check(bool ok, const char* what, const Result* r = 0, uint32_t where = 0) {
    g_fx_n++;
    if (ok) {
        if (getenv("VP_TRACE")) printf("  fix test: %s\n", what);
        return;
    }
    g_fx_bad++;
    printf("  FIX TEST FAILED: %s", what);
    if (r) printf(" (fault %d %08x at %08x, popped %u, ebx esi edi ebp %08x %08x %08x %08x, returned %08x)", r->fault, r->code, r->eip,
                  r->pops, r->regs[0], r->regs[1], r->regs[2], r->regs[3], r->ret);
    if (where) printf(" (wrote %08x)", where);
    printf("\n");
}
static Result fx_run(const Ent& f, bool rewrite, std::initializer_list<uint32_t> w) {
    uint32_t words[18] = {};
    int i = 0;
    for (uint32_t x : w) words[i++] = x;
    g_pc = _PC_53;
    return run(f, rewrite, words);
}
static bool fx_clean(const Ent& f, const Result& r) {
    return !r.fault && r.pops == (f.fast ? 4u * (uint32_t)f.nstack : 0u) && r.regs[0] == 0x0b0b0b0b && r.regs[1] == 0x05050505 &&
           r.regs[2] == 0x0d0d0d0d && r.regs[3] == 0x0e0e0e0e;
}
static bool fx_broke(const Ent& f, const Result& r) { return !fx_clean(f, r); }
struct Span { const void* p; uint32_t n; };
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
static int fx_count(uint32_t tag) {
    int n = 0;
    for (uint32_t i = 0; i < g_log.n && i < LOG_MAX; i++) n += g_log.w[i] == tag;
    return n;
}
static CallLog g_fx_log;
static void fx_same(const Ent& f, std::initializer_list<uint32_t> w, const char* what) {
    g_fx_same_n++;
    mem_save(g_snap);
    const Result ro = fx_run(f, false, w);
    mem_save(g_after);
    g_fx_log = g_log;
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
static struct { const Ent* f; bool rw; uint32_t w[18]; Result r; } g_fx_t;
static DWORD WINAPI fx_thread(void*) { g_pc = _PC_53; g_fx_t.r = run(*g_fx_t.f, g_fx_t.rw, g_fx_t.w); return 0; }
static bool fx_run_timed(const Ent& f, bool rewrite, std::initializer_list<uint32_t> w, Result* out) {
    g_fx_t.f = &f;
    g_fx_t.rw = rewrite;
    memset(g_fx_t.w, 0, sizeof g_fx_t.w);
    int i = 0;
    for (uint32_t x : w) g_fx_t.w[i++] = x;
    HANDLE h = CreateThread(0, 0x400000, fx_thread, 0, 0, 0);
    if (WaitForSingleObject(h, 2000) == WAIT_TIMEOUT) {
        TerminateThread(h, 0);
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    *out = g_fx_t.r;
    return true;
}
// the tests' world: a random one (from a fixed seed), the client's own task, no packets queued
static RaceClient* fx_world(bool multi) {
    mem_load(g_pristine);
    memset(g_arena, 0, ARENA_BYTES);
    g_logging = false;
    build_world(multi);
    g_logging = true;
    HState* s = HS();
    s->recv_n = 0;
    s->fail_mask = 0;
    RaceClient* c = g_rc;
    c->task = s->task_cur;
    return c;
}
static int32_t fx_uid(int slot) { return (int32_t)(((uint32_t)(slot + 0x11) << 8) | (uint32_t)slot); }
static uint8_t* fx_pkt(int t) {
    uint8_t* p = sv(0x100);
    memset(p, 0, 0x100);
    p[0] = 0x11;
    p[3] = (uint8_t)t;
    return p;
}

static void directed_fix_tests() {
    g_rng = 0x5eed4321u;
    char m[256];
    // ---- get_userdata: a user index past 7 --------------------------------------------------------------------------------
    {
        const Ent& f = fx_fn(0x004a8670);
        RaceClient* c = fx_world(false);
        for (uint32_t k : {0x2au, 0xc0u}) {
            const int32_t id = (int32_t)(0x3300u | k);
            uint8_t* at = (uint8_t*)c + 0x28 + k * 0x15;             // where the original looks: made to match
            memcpy(at, &id, 4);
            mem_save(g_snap);
            Result ro = fx_run(f, false, {U(c), 0, (uint32_t)id});
            sprintf(m, "get_userdata of index 0x%x: the original hands back memory past users[] as a user", k);
            fx_check(ro.fault || ro.ret == U(at), m, &ro);
            mem_load(g_snap);
            Result rn = fx_run(f, true, {U(c), 0, (uint32_t)id});
            sprintf(m, "get_userdata of index 0x%x: the rewrite finds nobody", k);
            fx_check(fx_clean(f, rn) && rn.ret == 0, m, &rn);
        }
        for (int i = 0; i < 8; i++) {
            c->users[i].id = fx_uid(i);
            sprintf(m, "get_userdata of user %d", i);
            fx_same(f, {U(c), 0, (uint32_t)fx_uid(i)}, m);
        }
    }
    // ---- dispatch_UserInfoPacket / dispatch_RemoveUserPacket: a user index past 7 ---------------------------------------------------
    for (uint32_t fn : {0x004a9b50u, 0x004a9bd0u}) {
        const Ent& f = fx_fn(fn);
        const char* nm = fn == 0x004a9b50u ? "a user info" : "a remove-user";
        RaceClient* c = fx_world(false);
        for (int i = 0; i < 8; i++) c->user_ids[i] = c->users[i].id = fx_uid(i);   // nothing to ask for after
        uint8_t* p = fx_pkt(fn == 0x004a9b50u ? 0x25 : 0x26);
        const int32_t id = 0x5500c0;                                 // index 0xc0: past the client
        memcpy(p + 4, &id, 4);
        strcpy((char*)p + 8, "intruder");
        const Span ok[] = {{&c->user_changes, 1}};
        mem_save(g_snap);
        Result ro = fx_run(f, false, {U(c), 0, U(p)});
        uint32_t ow = fx_outside(g_snap, {ok[0]});
        sprintf(m, "%s packet for index 0xc0: the original writes past users[] / user_ids[]", nm);
        fx_check(ro.fault || ow, m, &ro);
        mem_load(g_snap);
        Result rn = fx_run(f, true, {U(c), 0, U(p)});
        uint32_t nw = fx_outside(g_snap, {ok[0]});
        sprintf(m, "%s packet for index 0xc0: the rewrite drops it", nm);
        fx_check(fx_clean(f, rn) && !nw && fx_count('SREL') == 0, m, &rn, nw);
        mem_load(g_snap);
        const int32_t good = fx_uid(3);
        memcpy(p + 4, &good, 4);
        sprintf(m, "%s packet for user 3", nm);
        fx_same(f, {U(c), 0, U(p)}, m);
    }
    // ---- dispatch_NetCarInfoPacket: the car index -----------------------------------------------------------------------------------
    {
        const Ent& f = fx_fn(0x004a9930);
        RaceClient* c = fx_world(false);
        c->state = 0xd;
        const int32_t u = fx_uid(2);
        uint8_t* p = fx_pkt(0x33);
        memcpy(p + 5, &u, 4);
        p[4] = (uint8_t)(int8_t)-2;
        memcpy((uint8_t*)c + 0x7b8 - 8, &u, 4);                       // car_req[-2]: made to match
        memset((uint8_t*)c + 0xf0 - 2 * 0xd9, 0, 4);                  // cars[-2] (before the client): empty
        mem_save(g_snap);
        Result ro = fx_run(f, false, {U(c), 0, U(p)});
        uint32_t ow = fx_outside(g_snap, {{c, sizeof *c}});
        fx_check(ro.fault || ow, "a car info for car -2: the original copies it before the client", &ro);
        mem_load(g_snap);
        Result rn = fx_run(f, true, {U(c), 0, U(p)});
        uint32_t nw = fx_outside(g_snap, {});
        fx_check(fx_clean(f, rn) && !nw && g_log.n == 0, "a car info for car -2: the rewrite drops it", &rn, nw);
        mem_load(g_snap);
        for (int i = 0; i < 8; i++) c->car_req[i] = 0;
        c->car_req[3] = u;
        c->cars[3].user = 0;
        p[4] = 3;
        fx_same(f, {U(c), 0, U(p)}, "a car info for car 3");
    }
    // ---- dispatch_ChatPacket: "latency", the empty scrollback, a text with no NUL ----------------------------------------------------
    {
        const Ent& f = fx_fn(0x004a99e0);
        RaceClient* c = fx_world(true);
        c->users[1].id = fx_uid(1);
        strcpy(c->users[1].name, "talker");
        hs32(G_PACKET_DELAY, 5);
        uint8_t* p = fx_pkt(0x27);
        const int32_t id = fx_uid(1);
        memcpy(p + 4, &id, 4);
        strcpy((char*)p + 9, "latency");
        mem_save(g_snap);
        Result ro = fx_run(f, false, {U(c), 0, U(p)});
        fx_check(ro.fault, "a chat line \"latency\": the original's atoi reads through strchr's 0", &ro);
        mem_load(g_snap);
        Result rn = fx_run(f, true, {U(c), 0, U(p)});
        fx_check(fx_clean(f, rn) && hg32(G_PACKET_DELAY) == 5 && c->chat && !strcmp(c->chat->text, "latency"),
                 "a chat line \"latency\": the rewrite keeps the line, the delay as it was", &rn);
        // a text running to the end of the packet: "lat" its last 3 bytes, "ency 77" after it
        mem_load(g_snap);
        memset(p + 9, 'x', 0xe4 - 9);
        memcpy(p + 0xe1, "latency 77", 11);
        mem_save(g_snap);
        ro = fx_run(f, false, {U(c), 0, U(p)});
        fx_check(ro.fault || hg32(G_PACKET_DELAY) == 77, "a chat text with no NUL in the packet: the original reads on past it (a delay of 77)", &ro);
        mem_load(g_snap);
        rn = fx_run(f, true, {U(c), 0, U(p)});
        fx_check(fx_clean(f, rn) && hg32(G_PACKET_DELAY) == 5, "a chat text with no NUL in the packet: the rewrite reads no further", &rn);
        mem_load(g_snap);
        strcpy((char*)p + 9, "latency 120");
        fx_same(f, {U(c), 0, U(p)}, "a chat line \"latency 120\"");
        // the scrollback empty and its pool's allocation failing
        mem_load(g_snap);
        c->chat = 0;
        c->pool.block = 0;
        c->pool.free_list = 0;
        c->pool.used = 0;
        HS()->fail_mask = 0xffffffffu;
        HS()->alloc_calls = 0;
        strcpy((char*)p + 9, "hello");
        mem_save(g_snap);
        ro = fx_run(f, false, {U(c), 0, U(p)});
        fx_check(ro.fault, "a chat line with no room and the scrollback empty: the original reads the ring through 0", &ro);
        mem_load(g_snap);
        rn = fx_run(f, true, {U(c), 0, U(p)});
        fx_check(fx_clean(f, rn) && c->chat == 0, "a chat line with no room and the scrollback empty: the rewrite drops it", &rn);
    }
    // ---- race_packet: under 2 bytes --------------------------------------------------------------------------------------------------
    {
        const Ent& f = fx_fn(0x004a9de0);
        RaceClient* c = fx_world(false);
        HS()->paused = 0;
        for (int i = 0; i < 8; i++) c->cars[i].netcar = car_obj(8 + i);
        uint8_t* p = fx_pkt(0);
        p[0] = 0x12;
        mem_save(g_snap);
        Result ro;
        const bool done = fx_run_timed(f, false, {U(c), 0, U(p), 1}, &ro);
        fx_check(!done || fx_broke(f, ro), "a car packet of 1 byte: the original runs ~178 million records");
        mem_load(g_snap);
        Result rn = fx_run(f, true, {U(c), 0, U(p), 1});
        fx_check(fx_clean(f, rn) && fx_count('NEWP') == 0 && !fx_outside(g_snap, {{&c->last_race_pkt, 4}}),
                 "a car packet of 1 byte: the rewrite reads no records", &rn);
        mem_load(g_snap);
        for (int k = 0; k < 9; k++) p[2 + 24 * k] = (uint8_t)(k & 7);
        fx_same(f, {U(c), 0, U(p), 2 + 24 * 9}, "a car packet of 9 records");
        fx_same(f, {U(c), 0, U(p), 2}, "a car packet of 2 bytes (no records)");
    }
    // ---- DeityCast: longer than the packet --------------------------------------------------------------------------------------------
    {
        const Ent& f = fx_fn(0x004a8e80);
        RaceClient* c = fx_world(false);
        uint8_t* d = sv(0x200);
        memset(d, 0xee, 0x200);
        for (int32_t len : {0x100, -1}) {
            mem_save(g_snap);
            if (len > 0) {                                           // (-1: the original's copy runs over the whole stack, the harness's too)
                Result ro = fx_run(f, false, {U(c), 0, U(d), (uint32_t)len});
                sprintf(m, "a deity cast of %d bytes: the original copies it past its frame", len);
                fx_check(fx_broke(f, ro), m, &ro);
                mem_load(g_snap);
            }
            Result rn = fx_run(f, true, {U(c), 0, U(d), (uint32_t)len});
            sprintf(m, "a deity cast of %d bytes: the rewrite sends nothing", len);
            fx_check(fx_clean(f, rn) && fx_count('SREL') == 0, m, &rn);
            mem_load(g_snap);
        }
        fx_same(f, {U(c), 0, U(d), 0xe0}, "a deity cast of 0xe0 bytes");
    }
    // ---- GetPacketTypeString: outside its table ----------------------------------------------------------------------------------------
    {
        const Ent& f = fx_fn(0x004aa4f0);
        fx_world(false);
        mem_save(g_snap);
        Result ro = fx_run(f, false, {0x10000020u});
        fx_check(ro.fault || ro.ret != 0x004fd80c, "GetPacketTypeString of 0x10000020: the original reads far past its table", &ro);
        mem_load(g_snap);
        Result rn = fx_run(f, true, {0x10000020u});
        fx_check(fx_clean(f, rn) && rn.ret == 0x004fd80c, "GetPacketTypeString of 0x10000020: the rewrite gives PT_INVALID", &rn);
        for (uint32_t t = 0x20; t <= 0x3e; t++) {
            sprintf(m, "GetPacketTypeString of 0x%x", t);
            fx_same(f, {t}, m);
        }
    }
    // ---- clear_screen: no screen to grab --------------------------------------------------------------------------------------------------
    {
        const Ent& f = fx_fn(0x004a2700);
        fx_world(true);
        HS()->grab_never = 1;
        mem_save(g_snap);
        Result ro;
        const bool done = fx_run_timed(f, false, {U(g_arena + A_NAMES)}, &ro);
        fx_check(!done, "clear_screen with no screen to grab: the original never returns");
        mem_load(g_snap);
        Result rn = fx_run(f, true, {U(g_arena + A_NAMES)});
        fx_check(fx_clean(f, rn) && fx_count('GGRB') == 200 && fx_count('SPLS') == 1,
                 "clear_screen with no screen to grab: the rewrite gives up after 200 tries and draws the text", &rn);
        mem_load(g_snap);
        HS()->grab_never = 0;
        HS()->grab_fail_mask = 0xffffffffu;
        HS()->grab_calls = 0;
        fx_same(f, {U(g_arena + A_NAMES)}, "clear_screen with 32 failed grabs, then 4");
    }
    // ---- MultiSynchronize: no proposal ------------------------------------------------------------------------------------------------------
    {
        const Ent& f = fx_fn(0x004a2850);
        fx_world(true);
        HState* s = HS();
        LiveMultiInfo* lm = (LiveMultiInfo*)(g_arena + A_LMI);
        lm->server = 0;
        lm->owner = s->task_cur;
        *fcl_state(lm->client) = 0xf;
        s->cl_n = 2; s->cl_i = 0;
        s->cl_states[0] = 0xf; s->cl_states[1] = 0x12;
        s->no_proposal = 1;
        char* track = (char*)sv(0x40);
        strcpy(track, "unchanged");
        uint8_t* cars = sv(0xc84);
        mem_save(g_snap);
        Result ro = fx_run(f, false, {U(lm), U(cars), U(track)});
        fx_check(ro.fault, "MultiSynchronize with no proposal: the original reads the track through 0", &ro);
        mem_load(g_snap);
        Result rn = fx_run(f, true, {U(lm), U(cars), U(track)});
        fx_check(fx_clean(f, rn) && rn.ret == 1 && !strcmp(track, "unchanged") && fx_count('CSYN') == 1,
                 "MultiSynchronize with no proposal: the rewrite synchronizes, the track name as it was", &rn);
        mem_load(g_snap);
        s->no_proposal = 0;
        fx_same(f, {U(lm), U(cars), U(track)}, "MultiSynchronize with the proposal");
    }
    // ---- MultiAdjustPacketDelay: the sum past 32 bits -------------------------------------------------------------------------------------
    {
        const Ent& f = fx_fn(0x004a2ab0);
        fx_world(true);
        hs32(G_LMI, U(g_arena + A_LMI));
        hs32(G_PACKET_DELAY, 2000);
        mem_save(g_snap);
        Result ro = fx_run(f, false, {0x7fffffffu});
        fx_check(ro.fault || hg32(G_PACKET_DELAY) != 2000, "MultiAdjustPacketDelay(2000 + 2^31 - 1): the original's sum wraps (a delay of 0)", &ro);
        mem_load(g_snap);
        Result rn = fx_run(f, true, {0x7fffffffu});
        fx_check(fx_clean(f, rn) && hg32(G_PACKET_DELAY) == 2000, "MultiAdjustPacketDelay(2000 + 2^31 - 1): the rewrite's delay is 2000", &rn);
        for (int32_t d : {-3000, -2000, 0, 1, 2400}) {
            mem_load(g_snap);
            sprintf(m, "MultiAdjustPacketDelay(%d)", d);
            fx_same(f, {(uint32_t)d}, m);
        }
    }
    printf("the fix build: %d directed checks (%d boundary cases on both), %d failed\n", g_fx_n, g_fx_same_n, g_fx_bad);
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
    char* sl = strstr(exe, "\\test\\world_net_client.cpp");
    if (sl) strcpy(sl, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // the vtables of the fakes
    for (int i = 0; i < 32; i++) g_deity_vt[i] = (void*)dt_none;
    g_deity_vt[DT_START_RACE_AT / 4] = (void*)dt_StartRaceAt; g_deity_vt[DT_UPDATE / 4] = (void*)dt_Update;
    g_deity_vt[DT_CAR_IS_FINISHED / 4] = (void*)dt_CarIsFinished; g_deity_vt[DT_NET_CAST / 4] = (void*)dt_NetCast;
    for (int i = 0; i < 40; i++) g_fcl_vt[i] = (void*)fcl_none;
    g_fcl_vt[RC_DTOR / 4] = (void*)fcl_dtor; g_fcl_vt[RC_TICK / 4] = (void*)fcl_Tick; g_fcl_vt[RC_SET_OWNER / 4] = (void*)fcl_SetOwner;
    g_fcl_vt[RC_DISCONNECT / 4] = (void*)fcl_Disconnect; g_fcl_vt[RC_SPEAK / 4] = (void*)fcl_Speak;
    g_fcl_vt[RC_SYNCHRONIZE / 4] = (void*)fcl_Synchronize; g_fcl_vt[RC_WORLD_LOADED / 4] = (void*)fcl_WorldLoaded;
    g_fcl_vt[RC_GET_GREENFLAG_TIME / 4] = (void*)fcl_GetGreenflagTime; g_fcl_vt[RC_GET_RACE_INFO / 4] = (void*)fcl_GetRaceInfo;
    g_fcl_vt[RC_GET_PROPOSAL / 4] = (void*)fcl_GetProposal; g_fcl_vt[RC_GET_CAR_LIST / 4] = (void*)fcl_GetCarList;
    g_fcl_vt[RC_REGISTER_NETCAR / 4] = (void*)fcl_Register; g_fcl_vt[RC_REGISTER_LOCALCAR / 4] = (void*)fcl_Register;
    g_fcl_vt[RC_UNREGISTER / 4] = (void*)fcl_UnRegister; g_fcl_vt[RC_SEND_ALL / 4] = (void*)fcl_SendAll;
    g_fcl_vt[RC_CAR_IS_HUMAN / 4] = (void*)fcl_CarIsHuman; g_fcl_vt[RC_DEITY_CAST / 4] = (void*)fcl_DeityCast;
    for (int i = 0; i < 16; i++) g_fsv_vt[i] = (void*)fsv_none;
    g_fsv_vt[RS_DTOR / 4] = (void*)fsv_dtor; g_fsv_vt[RS_SET_OWNER / 4] = (void*)fsv_SetOwner; g_fsv_vt[RS_TICK / 4] = (void*)fsv_Tick;
    g_fsv_vt[RS_SEND_ALL / 4] = (void*)fsv_SendAll; g_fsv_vt[RS_EVERYONE_SYNCHRONIZED / 4] = (void*)fsv_EveryoneSync;
    g_fsv_vt[RS_RESTART_RACE / 4] = (void*)fsv_Restart; g_fsv_vt[RS_GO_BACK_TO_CHAT / 4] = (void*)fsv_BackToChat;
    // the stubs
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);         // _amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);         // _NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);         // _FF_MSGBANNER
    patch_jmp(0x004d5c10, (void*)&stub_crt_fatal);         // _fptrap
    struct { uint32_t at; void* to; } stubs[] = {
        {0x004140e0, (void*)&stub_MemAlloc}, {0x00414300, (void*)&stub_MemFree}, {0x00414390, (void*)&stub_delete},
        {0x00411150, (void*)&stub_LogReport}, {0x004112b0, (void*)&stub_LogPanic}, {0x00413b40, (void*)&stub_PTimeNow},
        {0x00414dd0, (void*)&stub_TaskGetID}, {0x00414ec0, (void*)&stub_TaskGetName}, {0x00414cb0, (void*)&stub_TaskSetPriority},
        {0x00414de0, (void*)&stub_TaskSleep}, {0x00414d90, (void*)&stub_TaskShouldISuspend}, {0x00414df0, (void*)&stub_TaskSuspendMe},
        {0x00414db0, (void*)&stub_TaskShouldIDie}, {0x004149e0, (void*)&stub_TaskCreate}, {0x00414d00, (void*)&stub_TaskSuspend},
        {0x00414d40, (void*)&stub_TaskResume}, {0x00414e40, (void*)&stub_TaskIsSuspended}, {0x00414ac0, (void*)&stub_TaskDestroy},
        {0x004150a0, (void*)&stub_MultiBegin}, {0x00415180, (void*)&stub_MultiEnter}, {0x004151d0, (void*)&stub_MultiLeave},
        {0x00415220, (void*)&stub_MultiEnd}, {0x00412bf0, (void*)&stub_Win32Idle}, {0x0041afb0, (void*)&stub_xlate},
        {0x004ceff0, (void*)&stub_atexit}, {0x0044e1c0, (void*)&stub_gxGrabScreen}, {0x0044fe30, (void*)&stub_gxSetCanvas},
        {0x0044ffd0, (void*)&stub_gxClear}, {0x0044e210, (void*)&stub_gxReleaseScreen}, {0x0044e180, (void*)&stub_gxFlip},
        {0x0040c9e0, (void*)&stub_SplashGeneric}, {0x0040a3d0, (void*)&stub_GetGameState}, {0x00426b20, (void*)&stub_PhysTaskRestart},
        {0x00426ea0, (void*)&stub_PTimeToPhys}, {0x0042bc00, (void*)&stub_PhysicsAbortRace},
        {0x00426e90, (void*)&stub_PhysicsGetDeviation}, {0x0042bd20, (void*)&stub_PhysicsIsPaused},
        {0x00464480, (void*)&stub_CarMgrCount}, {0x00464490, (void*)&stub_CarMgrGetInfo}, {0x0040e940, (void*)&stub_GhostCarIncognito},
        {0x00444580, (void*)&stub_FillNetPacket}, {0x0043f9c0, (void*)&stub_NewPacket}, {0x0040d540, (void*)&stub_HackHornBall},
        {0x00471500, (void*)&stub_OptionsGet}, {0x00465320, (void*)&stub_CarFileLoadSetup}, {0x00406810, (void*)&stub_GetTrackName},
        {0x00420c30, (void*)&stub_AIResetDriverMap}, {0x004aaac0, (void*)&stub_CreateTrackVersion},
        {0x004a76d0, (void*)&stub_SessionDiscReasonString}, {0x004a8060, (void*)&stub_ASSERT},
        {0x004a4f30, (void*)&sm_Ok}, {0x004a4f40, (void*)&sm_dtor}, {0x004a4fc0, (void*)&sm_CanDestroy}, {0x004a50d0, (void*)&sm_Tick},
        {0x004a55c0, (void*)&sm_GetServiceTable}, {0x004a5620, (void*)&sm_SRB}, {0x004a5f70, (void*)&sm_UConnect_id},
        {0x004a5fe0, (void*)&sm_UConnect_rs}, {0x004a60a0, (void*)&sm_Disconnect}, {0x004a6260, (void*)&sm_Shutdown},
        {0x004a6370, (void*)&sm_Send}, {0x004a6420, (void*)&sm_SendReliable}, {0x004a64c0, (void*)&sm_Recv},
        {0x004a66e0, (void*)&sm_GetStatus}, {0x004a75e0, (void*)&sm_GetDiscReason}, {0x004af4e0, (void*)&rdp_Latency},
    };
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    HS()->brk = A_HEAP;
    // the library's $E initialisers, once (the originals; atexit stubbed)
    for (auto& s : stubs) if (s.at == 0x004ceff0 || s.at == 0x0041afb0) patch_jmp(s.at, s.to);
    int ne = 0;
    g_logging = false;
    for (int i = 0; i < g_nfns; i++)
        if (g_fns[i].leftover && g_fns[i].name[0] == '$' && g_fns[i].name[1] == 'E') { call_orig(g_fns[i].v10, 0, {}); ne++; }
    g_logging = true;
    for (auto& s : stubs) patch_jmp(s.at, s.to);
    printf("ran the multi library's %d $E initialisers (the client's class mask %02x)\n", ne, hg8(G_CLASS_MASK));
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
    int differ = 0, fp_bad = 0, bad_fns = 0, nfn = 0, skipped = 0;
    int fixed_rounds = 0, fixed_bad = 0;                           // (the fix build: rounds kept out, a fixed case)
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (f.leftover) continue;
        if (only[0] && !strstr(f.name, only)) continue;
        nfn++;
        const bool multi = f.v10 < 0x004a7000;
        const int n = rounds * 40;
        bool fn_bad = false;
        int fn_checks = 0, fn_faults = 0;
        uint32_t shapes[64];
        int nshapes = 0;
        for (int rd = 0; rd < n && !fn_bad; rd++) {
            g_pc = (rd >> 1) & 1 ? _PC_24 : _PC_53;
            mem_load(g_pristine);
            memset(g_arena, 0, ARENA_BYTES);
            uint32_t words[18];
            g_logging = false;
            build_world(multi);
            const bool ok = args_for(f, words);
            g_logging = true;
            if (!ok) { skipped++; continue; }
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
            const bool pre_fixed = NET_FIXES && fx_pre_case(f, words);
            mem_save(g_snap);
            const Result ro = run(f, false, words);
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
                uint32_t h = 2166136261u;
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
            const Result rn = run(f, true, words);
            checks++; fn_checks++;
            log_words += g_log_orig.n;
            if (ro.fault) { faulted++; fn_faults++; }
            if (pre_fixed || (NET_FIXES && fx_post_case(f, ro, rn))) {   // a fixed case: not compared; the rewrite returns cleanly
                fixed_rounds++;
                const bool clean = !rn.fault && rn.pops == (f.fast ? 4u * (uint32_t)f.nstack : 0u) && rn.regs[0] == 0x0b0b0b0b &&
                                   rn.regs[1] == 0x05050505 && rn.regs[2] == 0x0d0d0d0d && rn.regs[3] == 0x0e0e0e0e;
                // (or a fault just where the original's: in an original callee, on the round's own damage)
                const bool same_fault = ro.fault && rn.fault && ro.code == rn.code && ro.eip == rn.eip;
                if (!clean && !same_fault) {
                    printf("  FIXED CASE %08x %s (round %d): the rewrite didn't return cleanly (fault %d %08x at %08x)\n", f.v10, f.name,
                           rd, rn.fault, rn.code, rn.eip);
                    fixed_bad++; fn_bad = true;
                }
                continue;
            }
            bool same = ro.fault == rn.fault && ro.code == rn.code && ro.ret == rn.ret && ro.pops == rn.pops &&
                        !memcmp(ro.regs, rn.regs, sizeof ro.regs) && ro.top == rn.top;
            const uint32_t where = mem_diff(g_after);
            same &= where == 0;
            same &= g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
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
                        if (where >= U(g_rc) && where < U(g_rc) + sizeof(RaceClient)) printf(" (RaceClient +%03x)", where - U(g_rc));
                        printf("\n");
                    } else printf("    memory first differs at %08x\n", where);
                }
                if (g_log.n != g_log_orig.n) printf("    the stub logs differ: %u / %u words\n", g_log_orig.n, g_log.n);
                for (uint32_t i = 0; i < g_log.n && i < g_log_orig.n && i < LOG_MAX; i++)
                    if (g_log.w[i] != g_log_orig.w[i]) {
                        printf("    the stub logs differ at word %u: %08x / %08x (", i, g_log_orig.w[i], g_log.w[i]);
                        for (uint32_t k = i > 6 ? i - 6 : 0; k <= i; k++) printf(" %08x", g_log_orig.w[k]);
                        printf(" )\n");
                        break;
                    }
                differ++;
                fn_bad = true;
            }
        }
        bad_fns += fn_bad;
        if (trace) printf("%08x %-56s %s  %d checks, %d faulted, %d call-log shapes\n", f.v10, f.name, fn_bad ? "BAD" : "ok",
                          fn_checks, fn_faults, nshapes);
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    printf("%d functions, %d listed twice; %lld checks (%d skipped), %lld logged words compared, %lld checks where the original "
           "faulted (both alike unless counted below): %d differ, %d footprint violations; %d functions bad\n", nfn, dup, checks,
           skipped, log_words, faulted, differ, fp_bad, bad_fns);
#if NET_FIXES
    printf("the fix build: %d rounds reached a fixed case (kept out of the comparison), %d where the rewrite didn't return cleanly\n",
           fixed_rounds, fixed_bad);
    if (!only[0] || !strcmp(only, "fix")) directed_fix_tests();         // (VP_ONLY=fix: those alone)
    mem_load(g_pristine);
    return differ || fp_bad || dup || fixed_bad || g_fx_bad ? 1 : 0;
#else
    (void)fixed_rounds;
    return differ || fp_bad || dup ? 1 : 0;
#endif
}
