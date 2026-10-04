// world_net_server.cpp -- multiplayer stage N2, group B: every rewrite of hook/net_server.cpp (server.obj: RaceServer) and
// hook/net_ded.cpp (ded.obj: the dedicated server) against its original, outside the game.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_net_server.cpp
//        /Fo<dir>\ /Fe<dir>\world_net_server.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//        /STACK:0x800000,0x800000
//   run:   world_net_server.exe [rounds] [seed]     (VP_TRACE=1: one line per function; VP_ONLY=text: those only;
//          VP_DEBUG_WORLD=1: every fault's registers)
//
// Loads out\race_v10.exe at 0x400000 the way test/world_net_core.cpp does (a child process with the range reserved) and
// includes the two files with PORT_FN redefined to list each function. VP_FAITHFUL. Once: every $E initialiser of ded.obj
// and server.obj (the originals: server.obj's class mask, its protocol table).
//
// For each function, `rounds` x 40 times, a world is BUILT in a 1 MB arena: a fake SessionMgr (its channels, its socket --
// MakeStr / GetHeaderSize / GetBPS), a RaceServer made by the ORIGINAL constructor over it and then made random -- users in
// any of the 8 slots (ids as create_userid makes them, names sometimes alike, every status, sync state from none to 40
// samples, round trips either side of 125 ms, clocks and keepalives either side of their limits), cars (the users' and
// strangers', human or AI, their latest records), the proposal, the AI count, the IROC user, the chat callback, the track
// version, the state 1..8, the owner task --, a dedicated Server over it, and a script: the packets UnboundRecv hands out
// (every game packet type, well formed or not -- names matching, versions matching or not, approvals for this proposal or
// another, car infos for the sender, nobody or a stranger, sync replies first-sent or resent, deity casts, car packets of
// 0..5 records for the sender's cars and others', unknown types), the sessions GetNextStatus reports down, GetStatus's
// answers, the console's lines (prompt answers, quit, server: commands, restart, end race, help, ?, junk; trailing blanks;
// lines filling the buffer), when TaskShouldIDie says yes, when the main loop's SessionMgr::Tick sets quit, when
// CanDestroySafely says yes, the file TrackCRC reads, which allocations fail. Then arguments made for the function. The
// stack the call uses is filled with one pattern first. The ORIGINAL runs from a raw call thunk; its results kept, the
// snapshot restored, the REWRITE runs (its callees: the originals). Compared: .data/.bss/.idata and the arena, the
// return value, bytes popped, ebx / esi / edi / ebp, the x87 stack depth, faults, and every stub's call log. A function
// that isn't replay_only may change only its footprint (checked on the original's pass; the harness state excepted).
//
// Stubbed (logged; their state in the arena): every SessionMgr method the server calls (packets by content from byte 3 --
// bytes 0-2 are the SessionMgr's own --, car packets from byte 2), CreateSessionMgr, SocketCreateUDP, GetEstRTLatency,
// MemAlloc / operator delete, LogReport / LogPanic / VERBOSE (formats with their arguments, strings by content),
// ASSERT_MSG (its arguments by the format), PTimeNow, the task calls, the track / lap / setup / file / resource / log-hook
// calls, sprintf / stricmp / strnicmp (the real ones, logged), and the console's KERNEL32 import slots (ReadConsoleA from
// the script; Sleep takes the main loop's part: it clears Server::g_msg). The game's own code everywhere else: the C
// runtime's strstr / isdigit / isspace / atoi / tolower / memmove / qsort / __alldiv, and every function of this group.
// Inputs the original can't survive (the FIX CANDIDATEs in the two files: out-of-range wire indices, a car count over 8,
// the empty-server next_user, get_numeric's 12 digits, SendAll's 8 records) are kept out of the worlds.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>
#include <type_traits>
#include <utility>

#define VP_FAITHFUL
#include "../hook/port.h"

// ---- the registry -----------------------------------------------------------------------------------------------------------
struct Ent {
    uint32_t v10;
    const char* name;
    void* fn;
    int fast, nstack, ret;
    void (*fp)(Footprint&, const uint32_t*);
};
static Ent g_fns[256];
static int g_nfns;
struct EntReg { EntReg(Ent e) { if (g_nfns < 256) g_fns[g_nfns++] = e; } };
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

// what net_wsock.h gives the rewrites: here, N0's sites unpatched (the plain call)
int __cdecl net_PTimeNow() { return ((int(__cdecl*)())(uintptr_t)0x00413b40)(); }

#include "../hook/net_server.cpp"
#include "../hook/net_ded.cpp"

using namespace nsv;
static uint8_t hg8(uint32_t a) { return *(volatile uint8_t*)(uintptr_t)a; }

// ---- random numbers -----------------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static int irange(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); }

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
static void set_iat(uint32_t slot, void* to) { *(uint32_t*)(uintptr_t)slot = (uint32_t)(uintptr_t)to; }

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
enum : uint32_t { A_STATE = 0x0, A_SM = 0x400, A_SESS = 0x500, A_SOCK = 0x700, A_SERVER = 0x780, A_RECVQ = 0x1000,
                  A_LINES = 0x2000, A_SCRATCH = 0x6000, A_HEAP = 0x20000 };
enum { NRECV = 16, NLINES = 8, LINE_STRIDE = 0x440 };
struct HState {
    uint32_t brk, alloc_calls, fail_mask;       // MemAlloc: a bump allocator; call k (k < 32) fails when bit k is set
    int32_t now, step;                          // PTimeNow: now += step each call
    int32_t task_cur;                           // TaskGetID
    uint32_t sm_ok, sm_create_ok, sock_create_ok;
    uint32_t service;                           // OfferUnboundService's answer, and the sessions [lo, lo + n) it sets
    int32_t lo;
    int32_t hdr, bps;                           // the socket's GetHeaderSize / GetBPS
    uint32_t nstat, istat;                      // GetNextStatus's script
    struct { int32_t sess; uint32_t st; } stat[8];
    uint32_t nrecv, irecv;                      // UnboundRecv's (RecvPkt at A_RECVQ)
    uint8_t conn[16];                           // GetStatus's answers, by session
    uint32_t nlines, iline, read_fail;          // ReadConsoleA's (Line at A_LINES); read k fails when bit k is set
    int32_t die_after, quit_after, destroy_after;
    uint32_t file_handle, file_ok;
};
static HState* HS() { return (HState*)(g_arena + A_STATE); }
struct RecvPkt { int32_t sess; int32_t len; uint8_t data[0xf0]; };
static_assert(sizeof(RecvPkt) <= 0x100, "RecvPkt");
static RecvPkt* recvq(uint32_t i) { return (RecvPkt*)(g_arena + A_RECVQ + 0x100 * i); }
struct Line { int32_t len; char data[LINE_STRIDE - 4]; };
static Line* line_k(uint32_t i) { return (Line*)(g_arena + A_LINES + LINE_STRIDE * i); }

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
#define U(p) ((uint32_t)(uintptr_t)(p))

// ---- stubs: the game ------------------------------------------------------------------------------------------------------------
static void __cdecl stub_LogReport(const char* fmt, ...) { L('LREP'); L_fmt(fmt, (const uint32_t*)(&fmt + 1)); }
static void __cdecl stub_LogPanic(const char* fmt, ...) { L('LPAN'); L_fmt(fmt, (const uint32_t*)(&fmt + 1)); }
static void __cdecl stub_VERBOSE(const char* fmt, ...) { L('VERB'); L_fmt(fmt, (const uint32_t*)(&fmt + 1)); }
// ASSERT_MSG (a bare `ret` in the game): its arguments, as many as its format's call passes
static void __cdecl stub_ASSERT(int cond, const char* fmt, ...) {
    const uint32_t* a = (const uint32_t*)(&fmt + 1);
    L('ASRT'); L((uint32_t)cond); L(U(fmt));
    const uint32_t f = U(fmt);
    int n = 4;                                                  // the owner checks: file, line, the two task names
    if (f == 0x004fd870) n = 0;                                 // "Invalid UserID used"
    if (f == 0x004fdbfc || f == 0x004fdc50 || f == 0x004fdca8 || f == 0x004fdd04) n = 1;   // the state's
    if (f == 0x004fe0a0) n = 2;                                 // Tick's
    for (int i = 0; i < n; i++) L(a[i]);
}
static void* __cdecl stub_MemAlloc(int n) {
    HState* s = HS();
    const uint32_t k = s->alloc_calls++;
    L('MALC'); L((uint32_t)n);
    if (n < 0 || n > 0x40000 || (k < 32 && (s->fail_mask >> k & 1))) return 0;
    const uint32_t at = (s->brk + 15) & ~15u;
    if (at + (uint32_t)n > ARENA_BYTES - 0x100) return 0;
    s->brk = at + (uint32_t)n;
    memset(g_arena + at, 0xcd, (size_t)n);
    return g_arena + at;
}
static void __cdecl stub_delete(void* p) { L('DEL '); L(U(p)); }
static int __cdecl stub_PTimeNow() { HState* s = HS(); s->now += s->step; L('PTIM'); return s->now; }
static int __cdecl stub_TaskGetID() { L('TKID'); return HS()->task_cur; }
static char g_task_names[4][16] = {"main", "lobby", "physics", "other"};
static char* __cdecl stub_TaskGetName(int id) { L('TKNM'); L((uint32_t)id); return g_task_names[(uint32_t)id & 3]; }
static void __cdecl stub_TaskSleep(int ms) { L('TSLP'); L((uint32_t)ms); }
static int __cdecl stub_TaskCreate(const char* name, void* fn) { L('TKCR'); L_str(name); L(U(fn)); return 77; }
static void __cdecl stub_TaskDestroy(int id) { L('TKDS'); L((uint32_t)id); }
static uint8_t __cdecl stub_TaskShouldIDie() { HState* s = HS(); L('TDIE'); return s->die_after-- <= 0 ? 1 : 0; }
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    L('SPRF'); L(U(fmt)); L_str(buf);
    return r;
}
static int __cdecl stub_stricmp(const char* a, const char* b) { L('STRI'); L_str(a); L_str(b); return _stricmp(a, b); }
static int __cdecl stub_strnicmp(const char* a, const char* b, size_t n) { L('STRN'); L_str(a); L_str(b); L((uint32_t)n); return _strnicmp(a, b, n); }
static const char* g_tracks[8] = {"Bemidji", "Kenyon", "Hawaii", "Nfield", "Arena", "Coliseum", "Rotunda", "Temple"};
static const char* __cdecl stub_GetTrackName(int i) { L('TRKN'); L((uint32_t)i); return g_tracks[(uint32_t)i & 7]; }
static int __cdecl stub_GetLapCount(int type, const char* name) { L('LAPS'); L((uint32_t)type); L_str(name); return type * 3 + 1; }
static void __cdecl stub_CarSetup(uint8_t* p) { L('CSET'); for (int i = 0; i < 0x8c; i++) p[i] = (uint8_t)(i * 7 + 1); }
static int __cdecl stub_FileOpen(const char* path) { L('FOPN'); L_str(path); return (int)HS()->file_handle; }
static uint8_t __cdecl stub_FileReadExact(int fh, uint8_t* buf, int n) {
    L('FRDX'); L((uint32_t)fh); L((uint32_t)n);
    for (int i = 0; i < n; i++) buf[i] = (uint8_t)(i * 13 + fh);
    return (uint8_t)(HS()->file_ok & 1);
}
static void __cdecl stub_FileClose(int* fh) { L('FCLS'); L((uint32_t)*fh); *fh = 0; }
static void __cdecl stub_LogInstallHook(void* fn) { L('LHKI'); L(U(fn)); }
static void __cdecl stub_LogUninstallHook(void* fn) { L('LHKU'); L(U(fn)); }
static const char* __cdecl stub_VersionString() { L('VERS'); return "v1.0 harness"; }
static void __cdecl stub_UsefulBegin() { L('UBEG'); }
static void __cdecl stub_UsefulEnd() { L('UEND'); }
static void __cdecl stub_ResMustLoad(const char* s) { L('RMLD'); L_str(s); }
static void __cdecl stub_ResUnload(const char* s) { L('RUNL'); L_str(s); }
static void __cdecl stub_RaceBegin() { L('RBEG'); }
static void __cdecl stub_RaceEnd() { L('REND'); }
static void __cdecl stub_chat_cb(void* data, int user, const char* text) { L('CHCB'); L(U(data)); L((uint32_t)user); L_str(text); }
static void __cdecl stub_crt_fatal(int code) { printf("the game's CRT reached a fatal path (%d): ending the test\n", code); fflush(stdout); ExitProcess(3); }

// ---- stubs: the SessionMgr, the socket ------------------------------------------------------------------------------------------
struct World {
    uint8_t* sock;
    netc::SessionMgr* sm;
    RaceServer* rs;
    Server* srv;
};
static World W;
static void L_pkt(const uint8_t* p, int len, int from) {
    if (len <= from || len > 0x400) { L((uint32_t)len); return; }
    L_bytes(p + from, (uint32_t)(len - from));
}
static void __fastcall sm_Tick(void*, int) {
    HState* s = HS();
    L('SMTK');
    if (s->quit_after > 0 && --s->quit_after == 0) *(volatile uint8_t*)(uintptr_t)G_QUIT = 1;
}
static uint8_t __fastcall sm_Ok(void*, int) { L('SMOK'); return (uint8_t)(HS()->sm_ok & 1); }
static void __fastcall sm_dtor(void* self, int) { L('SMDT'); L(U(self)); }
static void __fastcall sm_Shutdown(void*, int) { L('SMSD'); }
static uint8_t __fastcall sm_CanDestroy(void*, int) { L('SMCD'); return HS()->destroy_after-- <= 0 ? 1 : 0; }
static uint32_t __fastcall sm_Offer(void*, int, uint8_t* ls, int n) {
    HState* s = HS();
    L('SMOF'); L((uint32_t)n);
    L_str((const char*)ls);
    uint32_t t, v;
    memcpy(&t, ls + 0x20, 4); memcpy(&v, ls + 0x24, 4);
    L(t); L(v); L(ls[0x2c]);
    if (ls[0x2c]) L_str((const char*)ls + 0x34);
    const int16_t lo = (int16_t)s->lo, hi = (int16_t)(s->lo + n);
    memcpy(ls + 0x30, &lo, 2); memcpy(ls + 0x32, &hi, 2);
    return s->service;
}
static void __fastcall sm_Refuse(void*, int, uint32_t svc) { L('SMRF'); L(svc); }
static void __fastcall sm_Accept(void*, int, uint32_t svc) { L('SMAC'); L(svc); }
static void __fastcall sm_Withdraw(void*, int, uint32_t svc) { L('SMWD'); L(svc); }
static void __fastcall sm_Disconnect(void*, int, int sess, int why) { L('SMDC'); L((uint32_t)sess); L((uint32_t)why); }
static void __fastcall sm_DisconnectService(void*, int, uint32_t svc, int x) { L('SMDS'); L(svc); L((uint32_t)x); }
static void __fastcall sm_Send(void*, int, int sess, const uint8_t* p, int len, uint32_t flag) {
    L('SEND'); L((uint32_t)sess); L((uint32_t)len); L(flag & 0xff);
    L_pkt(p, len, 2);
}
static void __fastcall sm_SendReliable(void*, int, int sess, const uint8_t* p, int len, const uint32_t* rpi, uint32_t flag) {
    L('SNDR'); L((uint32_t)sess); L((uint32_t)len); L(flag & 0xff);
    if (rpi) { L(rpi[0]); L(rpi[1]); L(rpi[2]); } else L(0);
    L_pkt(p, len, 3);
}
static int __fastcall sm_UnboundRecv(void*, int, uint32_t svc, uint8_t* pkt, int* len) {
    HState* s = HS();
    L('URCV'); L(svc); L((uint32_t)*len);
    if (s->irecv >= s->nrecv) return -1;
    RecvPkt* r = recvq(s->irecv++);
    const int n = r->len < *len ? r->len : *len;
    memcpy(pkt, r->data, n > 0 ? n : 0);
    *len = n;
    return r->sess;
}
static uint8_t __fastcall sm_GetStatus(void*, int, int sess) { L('SMGS'); L((uint32_t)sess); return HS()->conn[sess & 15]; }
static int __fastcall sm_GetNextStatus(void*, int, uint8_t* st, uint32_t svc) {
    HState* s = HS();
    L('SMNS'); L(svc);
    if (s->istat >= s->nstat) return -1;
    *st = (uint8_t)s->stat[s->istat].st;
    return s->stat[s->istat++].sess;
}
static int __fastcall rdp_GetEstRTLatency(void* rdp, int, const void* addr) { L('RTLT'); L(U(rdp)); L(U(addr)); return (int)(U(addr) & 0xfff) * 7; }
static void* __cdecl stub_CreateSessionMgr(void* sock, int a, int b, int c) {
    L('CRSM'); L(U(sock)); L((uint32_t)a); L((uint32_t)b); L((uint32_t)c);
    return HS()->sm_create_ok & 1 ? (void*)W.sm : 0;
}
static void* __cdecl stub_SocketCreateUDP(uint32_t port) { L('CRSK'); L(port & 0xffff); return HS()->sock_create_ok & 1 ? (void*)W.sock : 0; }
static void __fastcall so_MakeStr(void*, int, const uint8_t* addr, char* buf) {
    L('SMKS'); L_bytes(addr, 12);
    sprintf(buf, "%02x%02x%02x%02x:%02x%02x", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}
static int __fastcall so_GetHeaderSize(void*, int) { L('SHDR'); return HS()->hdr; }
static int __fastcall so_GetBPS(void*, int) { L('SBPS'); return HS()->bps; }
static void* g_sock_vt[16] = {0, 0, 0, 0, 0, 0, (void*)so_MakeStr, 0, 0, 0, 0, (void*)so_GetHeaderSize, (void*)so_GetBPS, 0, 0, 0};

// ---- stubs: the console (KERNEL32, __stdcall) ------------------------------------------------------------------------------------
static int WINAPI k_AllocConsole() { L('ACON'); return 1; }
static uint32_t WINAPI k_GetStdHandle(uint32_t n) { L('GSTD'); L(n); return 0x100 + (n & 0xff); }
static int WINAPI k_SetConsoleMode(uint32_t h, uint32_t m) { L('SCMD'); L(h); L(m); return 1; }
static int WINAPI k_SetConsoleTitleA(const char* s) { L('SCTT'); L_str(s); return 1; }
static int WINAPI k_WriteConsoleA(uint32_t h, const void* buf, uint32_t n, uint32_t* written, void* r) {
    L('WRCN'); L(h); L_bytes(buf, n); L(U(r));
    if (written) *written = n;
    return 1;
}
static int WINAPI k_ReadConsoleA(uint32_t h, char* buf, uint32_t size, uint32_t* n, void* r) {
    HState* s = HS();
    L('RDCN'); L(h); L(size); L(U(r));
    const uint32_t k = s->iline;
    if (k < 32 && (s->read_fail >> k & 1)) { s->iline++; L('FAIL'); return 0; }
    if (k >= s->nlines) { *(volatile uint8_t*)(uintptr_t)G_QUIT = 1; L('EOF '); return 0; }
    s->iline++;
    const Line* l = line_k(k);
    const uint32_t c = (uint32_t)l->len < size ? (uint32_t)l->len : size;
    memcpy(buf, l->data, c);
    *n = c;
    L_bytes(buf, c);
    return 1;
}
static int WINAPI k_FreeConsole() { L('FCON'); return 1; }
static void WINAPI k_Sleep(uint32_t ms) { L('SLEP'); L(ms); *(volatile uint32_t*)(uintptr_t)G_MSG = 0; }   // the main loop took it

// ---- the raw call (world_net_core.cpp's) -------------------------------------------------------------------------------------------
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
__declspec(noinline) static Result run(const Ent& f, bool rewrite, const uint32_t* words) {
    Result r = {};
    g_log.n = 0;
    g_c_fn = rewrite ? U(f.fn) : f.v10;
    const uint32_t* stack = words;
    if (f.fast) { g_c_ecx = words[0]; g_c_edx = words[1]; stack = words + 2; }
    else { g_c_ecx = 0x00c0ffee; g_c_edx = 0x00d00d1e; }
    g_c_n = (uint32_t)f.nstack;
    for (int i = 0; i < f.nstack && i < 16; i++) g_c_args[i] = stack[i];
    unsigned cw;
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
    fill_stack();
    __try {
        raw_call();
    } __except (fault_filter(GetExceptionInformation())) { r.fault = 1; r.code = g_fault_code; r.eip = g_fault_eip; }
    __asm fninit
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
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
    Ent e = {fn, "(setup)", 0, fast, (int)words.size() - (fast ? 2 : 0), 4, 0};
    uint32_t w[16];
    int i = 0;
    for (uint32_t x : words) w[i++] = x;
    const Result r = run(e, false, w);
    if (r.fault) printf("setup: %08x faulted (%08x at %08x)\n", fn, r.code, r.eip);
    return r.ret;
}

// ---- the world -------------------------------------------------------------------------------------------------------------------
static uint32_t g_scratch_at;
static uint8_t* sv(uint32_t n) {                  // a scratch buffer, random-filled
    g_scratch_at = (g_scratch_at + 15) & ~15u;
    uint8_t* p = g_arena + A_SCRATCH + g_scratch_at;
    g_scratch_at += n;
    if (g_scratch_at > 0x19000) { printf("scratch overflow\n"); ExitProcess(4); }
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)rnd();
    return p;
}
static const char* const k_names[] = {"bob", "Bob", "alice", "ALICE", "x", "carl", "dave", "eleven_char", "twelve_chars"};
static void rand_name(char* d, int cap) {
    if (chance(70)) { strncpy(d, k_names[rnd() % 9], cap - 1); d[cap - 1] = 0; return; }
    const int n = irange(0, cap - 1);
    for (int i = 0; i < n; i++) d[i] = (char)('a' + rnd() % 26);
    d[n] = 0;
}
static int present(int i) { return W.rs->users[i].id != 0; }
static int any_user_idx() {
    int c[8], n = 0;
    for (int i = 0; i < 8; i++) if (present(i)) c[n++] = i;
    return n ? c[rnd() % n] : irange(0, 7);
}
static int32_t uid_of(int i) { return W.rs->users[i].id ? W.rs->users[i].id : (int32_t)(i | (HS()->lo + i) << 8 | 77 << 16); }

// a game packet of type t from user index ui (the server's view: sess = lo + ui); returns its length
static int make_game_packet(uint8_t* p, int t, int ui) {
    RaceServer* r = W.rs;
    for (int i = 0; i < 0xe4; i++) p[i] = (uint8_t)rnd();
    p[0] = (uint8_t)((chance(80) ? 2 : 3) << 4 | 1);
    p[3] = (uint8_t)t;
    switch (t) {
    case 0x21: {
        rand_name((char*)p + 4, 13);
        if (chance(90)) memcpy(p + 0x11, (const void*)(uintptr_t)G_PROTOCOL, 0x1f);
        if (chance(85)) memcpy(p + 0x30, r->track_version, 0x20);
        return 0x50;
    }
    case 0x22: case 0x34: case 0x39: return 4;
    case 0x24: { const int32_t id = chance(85) ? uid_of(any_user_idx()) : irange(0, 7); memcpy(p + 4, &id, 4); return 8; }
    case 0x28: {
        const int n = irange(0, 60);
        for (int i = 0; i < n; i++) p[4 + i] = (uint8_t)(' ' + rnd() % 90);
        p[4 + n] = 0;
        return 0x48;
    }
    case 0x29: {
        const int ti = any_user_idx();
        const int32_t id = chance(85) ? uid_of(ti) : irange(0, 7);
        memcpy(p + 4, &id, 4);
        const int n = irange(0, 60);
        for (int i = 0; i < n; i++) p[8 + i] = (uint8_t)(' ' + rnd() % 90);
        p[8 + n] = 0;
        return 0x4c;
    }
    case 0x2b: { const int32_t id = chance(70) ? r->prop.id : (int32_t)rnd(); memcpy(p + 4, &id, 4); return 8; }
    case 0x30: {
        int32_t id = chance(60) ? uid_of(ui) : chance(60) ? 0 : uid_of(any_user_idx());
        if (chance(5)) id = (int32_t)(rnd() & 0xff00ff00);
        memcpy(p + 5, &id, 4);
        p[0xdd] = 0;
        return 0xde;
    }
    case 0x32: p[4] = (uint8_t)irange(0, 7); return 8;
    case 0x38: { const int32_t w = (int32_t)rnd(); memcpy(p + 4, &w, 4); return 0x34; }
    case 0x3b: return 0x10;
    default: return irange(4, 0x40);
    }
}
static int make_race_packet(uint8_t* p, int ui, int* lenp) {
    RaceServer* r = W.rs;
    for (int i = 0; i < 0xe4; i++) p[i] = (uint8_t)rnd();
    p[0] = 0x72;
    const int k = irange(0, 5);
    for (int j = 0; j < k; j++) {
        int ci = irange(0, 7);
        if (chance(60))
            for (int c = 0; c < 8; c++) if (r->cars[c].info.user == W.rs->users[ui].id && W.rs->users[ui].id) { ci = c; break; }
        p[2 + 24 * j] = (uint8_t)ci;
    }
    *lenp = chance(90) ? 2 + 24 * k : 2 + 24 * k + irange(1, 23);
    if (*lenp > 0xe2) *lenp = 2 + 24 * 9;
    return *lenp;
}
static const int k_types[] = {0x21, 0x22, 0x24, 0x28, 0x29, 0x2b, 0x30, 0x32, 0x34, 0x38, 0x39, 0x3b, 0x23, 0x2a, 0x3c, 0x40};

static void build_world() {
    HState* s = HS();
    memset(s, 0, sizeof *s);
    s->brk = A_HEAP;
    s->now = (int32_t)(rnd() % 100000) + 30000;
    s->step = chance(20) ? 0 : irange(1, 300);
    s->task_cur = (int32_t)(rnd() & 3);
    s->lo = chance(80) ? 0 : irange(1, 4);
    s->service = chance(10) ? 0 : 1000 + rnd() % 50;
    s->sm_ok = chance(90); s->sm_create_ok = chance(90); s->sock_create_ok = chance(90);
    s->hdr = irange(20, 60);
    s->bps = irange(1000, 20000);
    if (chance(3)) { s->hdr = irange(-30, 30); s->bps = irange(0, 100); }
    g_scratch_at = 0;
    memset(&W, 0, sizeof W);
    W.sock = g_arena + A_SOCK;
    *(void***)W.sock = g_sock_vt;
    W.sm = (netc::SessionMgr*)(g_arena + A_SM);
    W.sm->sessions = (netc::SessionInfo*)(g_arena + A_SESS);
    W.sm->nsessions = 16;
    W.sm->sock = W.sock;
    for (int i = 0; i < 16 * 0x1c; i++) ((uint8_t*)W.sm->sessions)[i] = (uint8_t)rnd();
    // the RaceServer: the original constructor's, then random
    RaceServer* r = (RaceServer*)(uintptr_t)call_orig(F_MemAlloc, 0, {0x1198});
    char* nm = (char*)sv(40);
    rand_name(nm, 33);
    char* pw = chance(50) ? (char*)sv(16) : 0;
    if (pw) rand_name(pw, 16);
    call_orig(S_ctor, 1, {U(r), 0, U(W.sm), U(nm), U(pw)});
    W.rs = r;
    const int lo = r->base;
    for (int i = 0; i < 8; i++) {
        ServerData* u = &r->users[i];
        if (!chance(65)) continue;
        u->id = i | (lo + i) << 8 | irange(1, 50) << 16;
        rand_name(u->name, 13);
        u->status = irange(0, 6);
        u->dt = irange(-50000, 50000);
        u->sync_sent = s->now - irange(0, 500);
        for (int k = 0; k < 40; k++) { u->samples[k].rtt = (uint16_t)irange(0, 1000); u->samples[k].remote = (int32_t)rnd(); }
        u->nsamples = chance(50) ? irange(36, 39) : irange(0, 39);
        u->sync_acked = (uint8_t)(rnd() & 1);
        u->rtt = chance(50) ? irange(0, 125) : irange(126, 600);
        u->send_interval = irange(0, 200);
        u->last_send = s->now - irange(0, 400);
        u->last_heard = s->now - (chance(85) ? irange(0, 16000) : irange(16000, 20000));
    }
    int nusers = 0;
    for (int i = 0; i < 8; i++) nusers += present(i);
    r->ncars = irange(0, 6);
    for (int k = 0; k < r->ncars; k++) {
        ServerNetCarInfo* c = &r->cars[k];
        for (uint32_t b = 0; b < sizeof *c; b++) ((uint8_t*)c)[b] = (uint8_t)rnd();
        c->info.user = chance(85) ? uid_of(any_user_idx()) : (int32_t)rnd();
        c->info.user2 = irange(0, 7) | (int32_t)(rnd() & 0xff00);
        rand_name(c->info.driver, 13);
        rand_name(c->info.car, 32);
        c->info.human = (uint8_t)(rnd() & 1);
        c->rec[0] = (uint8_t)k;
        c->rec_time = s->now - irange(0, 500);
    }
    const int room = 8 - r->ncars;
    *(int32_t*)((uint8_t*)r + 0x1164) = nusers && chance(50) ? irange(0, room) : 0;
    for (uint32_t b = 0; b < sizeof r->prop; b++) ((uint8_t*)&r->prop)[b] = (uint8_t)rnd();
    *((uint8_t*)&r->prop + 4) = (uint8_t)(chance(50) ? 0 : 1);
    r->proposing = (uint8_t)(rnd() & 1);
    r->cars_locked = (uint8_t)(rnd() & 1);
    r->serial = irange(0, 100);
    r->iroc_user = chance(70) && nusers ? uid_of(any_user_idx()) : 0;
    r->chat_cb = chance(50) ? (void*)stub_chat_cb : 0;
    r->chat_data = (void*)(uintptr_t)rnd();
    if (chance(20)) for (int k = 0; k < 8; k++) r->track_version[k] = (int32_t)rnd();
    if (chance(5)) r->track_version[0] = -1;
    r->state = irange(1, 8);
    r->last_tick = chance(20) ? 0 : s->now - irange(0, 1000);
    r->owner = chance(85) ? s->task_cur : (s->task_cur + 1) & 3;
    // the dedicated Server over it
    W.srv = (Server*)(g_arena + A_SERVER);
    W.srv->sm = W.sm;
    W.srv->rs = r;
    for (uint32_t b = 0; b < sizeof W.srv->prop; b++) ((uint8_t*)&W.srv->prop)[b] = (uint8_t)rnd();
    W.srv->prop.track = irange(0, 7);
    W.srv->prop.race_type = irange(0, 3);
    // the scripts
    for (int i = 0; i < 16; i++) s->conn[i] = (uint8_t)(rnd() % 4);
    s->nstat = (uint32_t)irange(0, 3);
    for (uint32_t i = 0; i < s->nstat; i++) { s->stat[i].sess = lo + irange(0, 7); s->stat[i].st = (uint32_t)(chance(50) ? 1 : rnd() & 0xff); }
    s->nrecv = (uint32_t)irange(0, 8);
    for (uint32_t i = 0; i < s->nrecv; i++) {
        RecvPkt* q = recvq(i);
        const int ui = chance(85) ? any_user_idx() : irange(0, 7);
        q->sess = lo + ui;
        if (chance(20)) { int len; make_race_packet(q->data, ui, &len); q->len = len; }
        else q->len = make_game_packet(q->data, k_types[rnd() % 16], ui);
        if (chance(5)) q->data[0] = (uint8_t)rnd();
    }
    s->die_after = irange(0, 6);
    s->quit_after = 0;
    s->destroy_after = irange(0, 3);
    s->file_handle = chance(80) ? 5 : 0;
    s->file_ok = chance(80);
    s->read_fail = chance(20) ? rnd() & rnd() : 0;
    s->alloc_calls = 0;
    s->fail_mask = chance(10) ? rnd() & rnd() : 0;
    s->now += irange(0, 300);
    *(uint8_t*)(uintptr_t)G_QUIT = 0;
    *(uint8_t*)(uintptr_t)G_CHAT_CONFIG = (uint8_t)(rnd() & 1);
    *(uint32_t*)(uintptr_t)G_MSG = 0;
    *(uint32_t*)(uintptr_t)G_GLOBAL = U(W.srv);
    *(uint32_t*)(uintptr_t)G_HIN = 0x1f6;
    *(uint32_t*)(uintptr_t)G_HOUT = 0x1f5;
}

// console lines
static const char* const k_cmds[] = {"quit", "QUIT now", "server: track 3", "server: aicars 2 simulation", "restart", "end race",
                                     "help", "?", "hello", "", "server:", "Server: long hard ai damage on reversed", "y", "n",
                                     "No", "Viper Test", "secret"};
static void add_line(const char* s, bool blanks) {
    HState* h = HS();
    if (h->nlines >= NLINES) return;
    Line* l = line_k(h->nlines++);
    int n = (int)strlen(s);
    memcpy(l->data, s, n);
    if (blanks) { const int b = irange(1, 3); for (int i = 0; i < b; i++) l->data[n++] = chance(50) ? ' ' : '\n'; }
    l->len = n;
}
static void add_random_lines(int k) {
    for (int i = 0; i < k; i++) {
        if (chance(10)) {                                       // a long one (fills idle's 0x400 buffer)
            HState* h = HS();
            if (h->nlines >= NLINES) return;
            Line* l = line_k(h->nlines++);
            l->len = chance(50) ? 0x400 : irange(0x20, 0x420);
            for (int j = 0; j < l->len; j++) l->data[j] = (char)('a' + rnd() % 26);
            if (chance(50)) l->data[l->len - 1] = ' ';
            if (l->len >= 0x400) l->data[0x3ff] = ' ';      // FIX CANDIDATE kept out: idle's full read has no NUL
            continue;
        }
        add_line(k_cmds[rnd() % (sizeof k_cmds / sizeof *k_cmds)], chance(50));
    }
}
// track_chat's language
static char* chat_text() {
    char* t = (char*)sv(200);
    t[0] = 0;
    if (chance(90)) strcat(t, chance(80) ? "server: " : "Server: ");
    static const char* const words[] = {"arcade", "intermediate realism", "simulation", "sprint", "short", "long", "insane",
                                        "easy ai", "intermediate ai", "hard ai", "damage off", "damage on", "forward",
                                        "reversed", "end race", "restart", "junk"};
    const int n = irange(0, 4);
    for (int i = 0; i < n; i++) {
        char b[48];
        switch (rnd() % 4) {
        case 0: sprintf(b, "track %d ", irange(-2, 12)); break;
        case 1: sprintf(b, "aicars %d ", irange(-1, 9)); break;
        case 2: sprintf(b, "track %s ", chance(50) ? "x" : "99999999999"); break;
        default: sprintf(b, "%s ", words[rnd() % 17]); break;
        }
        if (strlen(t) + strlen(b) < 190) strcat(t, b);
    }
    return t;
}

// ---- arguments per function ---------------------------------------------------------------------------------------------------------
static bool args_for(const Ent& f, uint32_t* w) {
    RaceServer* r = W.rs;
    HState* s = HS();
    const int lo = r->base;
    for (int i = 0; i < 18; i++) w[i] = 0;
    w[0] = U(r);
    int ui = chance(90) ? any_user_idx() : irange(0, 7);
    if (f.v10 == S_dispatch_AddMe && chance(60)) {               // a new user: an empty slot (the session layer's)
        int c[8], k = 0;
        for (int i = 0; i < 8; i++) if (!present(i)) c[k++] = i;
        if (k) ui = c[rnd() % k];
    }
    const int sess = lo + ui;
    // the checks and the main loop: often everyone in the state that moves the machine on
    if (chance(50) && (f.v10 == S_checkRS_APPROVE || f.v10 == S_checkRS_CLOSE_ENTRY || f.v10 == S_checkRS_CHOOSE_CAR ||
                       f.v10 == S_checkRS_GET_CAR || f.v10 == S_checkRS_SYNC || f.v10 == S_checkRS_PRESTAGE || f.v10 == S_Tick ||
                       f.v10 == D_Server_Tick || f.v10 == 0x004a36a0)) {
        static const int k_next[9] = {0, 1, 1, 2, 4, 5, 6, 6, 6};
        const int st = chance(80) ? k_next[r->state] : irange(0, 6);
        for (int i = 0; i < 8; i++) if (present(i)) r->users[i].status = chance(90) ? st : irange(0, 6);
        if (f.v10 == S_checkRS_SYNC) for (int i = 0; i < 8; i++) if (present(i) && chance(50)) r->users[i].status = irange(5, 7);
    }
    switch (f.v10) {
    // ---- server.obj -----------------------------------------------------------------------------------------------------
    case S_smart_strncpy: {
        char* src = (char*)sv(80);
        src[irange(0, 79)] = 0;
        w[0] = U(sv(64)); w[1] = chance(95) ? U(src) : 0; w[2] = (uint32_t)irange(-2, 60);
        return true;
    }
    case S_TrackCRC: { char* n = (char*)sv(16); rand_name(n, 12); w[0] = U(n); return true; }
    case S_CreateTrackVersion: w[0] = U(sv(32)); return true;
    case S_ServerData_reset: w[0] = U(&r->users[irange(0, 7)]); return true;
    case S_ctor: {
        char* nm = (char*)sv(48);
        if (chance(80)) rand_name(nm, 40); else nm[47] = 0;
        char* pw = (char*)sv(16);
        rand_name(pw, 16);
        w[0] = U(sv(0x1198)); w[2] = U(W.sm); w[3] = U(nm); w[4] = chance(50) ? U(pw) : 0;
        return true;
    }
    case S_CreateRaceServer: {
        char* nm = (char*)sv(48);
        rand_name(nm, 40);
        w[0] = U(W.sm); w[1] = U(nm); w[2] = chance(50) ? U(sv(8)) : 0;
        if (w[2]) *(char*)(uintptr_t)w[2] = chance(50) ? 'p' : 0, ((char*)(uintptr_t)w[2])[1] = 0;
        return true;
    }
    case S_everyone_in_state: w[2] = (uint32_t)irange(0, 6); w[3] = chance(80) ? U(sv(4)) : 0; return true;
    case S_everyone_approved: w[2] = chance(80) ? U(sv(4)) : 0; return true;
    case S_user_disconnected: w[2] = (uint32_t)irange(0, 7); return true;
    case S_next_user: {
        int nu = 0;
        for (int i = 0; i < 8; i++) nu += present(i);
        if (!nu) return false;                                   // FIX CANDIDATE: never returns
        w[2] = chance(30) ? 0 : (uint32_t)uid_of(any_user_idx());
        return true;
    }
    case S_add_ai_cars: case S_CHOOSE_CAR_GET_CAR: case S_checkRS_CHOOSE_CAR: return true;
    case S_broadcast: { uint8_t* p = sv(64); w[2] = U(p); w[3] = (uint32_t)irange(4, 64); return true; }
    case S_create_userid: w[2] = (uint32_t)sess; return true;
    case S_make_unique_username: {
        char* n = (char*)sv(16);
        if (chance(60) && present(ui)) {
            strcpy(n, r->users[ui].name);
            if (chance(50)) for (char* c = n; *c; c++) *c = (char)toupper(*c);
        } else rand_name(n, 13);
        w[2] = U(n); w[3] = (uint32_t)irange(0, 7);
        return true;
    }
    case S_send_userlist: w[2] = (uint32_t)sess; w[3] = (uint32_t)uid_of(ui); return true;
    case S_rpi_cb: {
        uint32_t* d = (uint32_t*)sv(8);
        d[0] = U(r); d[1] = (uint32_t)irange(0, 7);
        w[0] = rnd() & (chance(50) ? 0xff : 0xffffffffu); w[1] = U(d);
        return true;
    }
    case S_sync_rpi_cb: w[2] = (uint32_t)irange(0, 7); w[3] = rnd(); return true;
    case S_calc_send_interval: w[2] = U(&r->users[irange(0, 7)]); return true;
    case S_no_bogus_crcs: { int32_t* c = (int32_t*)sv(32); if (chance(30)) c[0] = -1; if (chance(30)) c[1] = -1; w[0] = U(c); return true; }
    case S_sync_sample_cmp: w[0] = U(sv(6)); w[1] = U(sv(6)); return true;
    case S_dispatch_AddMe: case S_dispatch_UserInfoReq: case S_dispatch_Speak: case S_dispatch_Whisper: case S_dispatch_Approve:
    case S_dispatch_NetCarInfo: case S_dispatch_NetCarInfoRequest: case S_dispatch_RaceReady: case S_dispatch_Sync:
    case S_dispatch_GoReady: case S_dispatch_KeepAlive: {
        const int t = f.v10 == S_dispatch_AddMe ? 0x21 : f.v10 == S_dispatch_UserInfoReq ? 0x24 : f.v10 == S_dispatch_Speak ? 0x28
                    : f.v10 == S_dispatch_Whisper ? 0x29 : f.v10 == S_dispatch_Approve ? 0x2b : f.v10 == S_dispatch_NetCarInfo ? 0x30
                    : f.v10 == S_dispatch_NetCarInfoRequest ? 0x32 : f.v10 == S_dispatch_RaceReady ? 0x34
                    : f.v10 == S_dispatch_Sync ? 0x38 : f.v10 == S_dispatch_GoReady ? 0x39 : 0x22;
        uint8_t* p = sv(0xe4);
        make_game_packet(p, t, ui);
        if (t == 0x32) {                                         // keep get_iroc_car's 0 out (FIX CANDIDATE): the IROC user has a car
            const int ci = p[4];
            if (*((uint8_t*)&r->prop + 4) && r->cars[ci].info.human) {
                bool has = false;
                for (int k = 0; k < r->ncars; k++) has |= r->cars[k].info.user == r->iroc_user && r->cars[k].info.human;
                if (!has) *((uint8_t*)&r->prop + 4) = 0;
            }
        }
        if (t == 0x30 && r->ncars >= 8) return false;
        if (t == 0x21) {                                         // a free slot for the new user (as the session layer has it)
            r->serial = irange(0, 100);
        }
        w[2] = U(p); w[3] = (uint32_t)sess;
        return true;
    }
    case S_deitycast: { uint8_t* p = sv(0x20); make_game_packet(p, 0x3b, ui); w[2] = U(p); w[3] = (uint32_t)sess; w[4] = 0x10; return true; }
    case S_race_packet: {
        uint8_t* p = sv(0xe4);
        int len;
        make_race_packet(p, ui, &len);
        w[2] = U(p); w[3] = (uint32_t)sess; w[4] = (uint32_t)len;
        return true;
    }
    case S_Propose: {
        NetProposal* p = (NetProposal*)sv(0x3c);
        w[2] = U(p); w[3] = chance(40) ? 0 : (uint32_t)uid_of(any_user_idx());
        return true;
    }
    case S_SetIrocCar: w[2] = (uint32_t)uid_of(any_user_idx()); w[3] = U(sv(8)); return true;
    case S_SetOwner: w[2] = rnd() & 7; return true;
    case D_chat_cb: case S_SetChatCallback:
        if (f.v10 == S_SetChatCallback) { w[2] = rnd(); w[3] = rnd(); return true; }
        break;
    case S_Tick: case S_SendAll: case S_dtor: case S_Ok: case S_check_for_dead_users: case S_check_for_singleton_player:
    case S_send_status_tab: case S_get_iroc_car: case S_reset: case S_RestartRace: case S_GoBackToChat: case 0x004ac070:
    case 0x004ac0f0: case S_APPROVE_CLOSE_ENTRY: case S_CLOSE_ENTRY_APPROVE: case S_CLOSE_ENTRY_CHOOSE_CAR:
    case S_CHOOSE_CAR_APPROVE: case S_GET_CAR_SYNC: case S_SYNC_PRESTAGE: case S_PRESTAGE_STAGE: case S_STAGE_RACE:
    case S_RACE_PRESTAGE: case S_RACE_APPROVE: case S_checkRS_APPROVE: case S_checkRS_CLOSE_ENTRY: case S_checkRS_GET_CAR:
    case S_checkRS_SYNC: case S_checkRS_PRESTAGE: case S_checkRS_STAGE: case 0x004acfe0: case 0x004ad040:
        return true;
    case 0x004ad130: case 0x004ad150: case 0x004ad180:
        if (chance(20)) r->state = irange(-1, 10);
        return true;
    default: break;
    }
    // ---- ded.obj ----------------------------------------------------------------------------------------------------------------
    w[0] = U(W.srv);
    switch (f.v10) {
    case D_init_game_info: case D_update_laps: return true;
    case D_Server_Tick:
        if (chance(50)) *(uint32_t*)(uintptr_t)G_MSG = U(chat_text());
        return true;
    case D_track_chat: case D_chat: w[2] = chance(50) ? 0 : (uint32_t)uid_of(any_user_idx()); w[3] = U(chat_text()); return true;
    case D_chat_cb: w[0] = rnd(); w[1] = chance(50) ? 0 : (uint32_t)uid_of(any_user_idx()); w[2] = U(chat_text()); return true;
    case D_contains_list: {
        uint32_t* l = (uint32_t*)sv(24);
        const uint32_t strs[] = {0x004fb5ec, 0x004fb5f4, 0x004fb60c, 0x004fb618, 0x004fb620, 0x004fb5e4};
        const int n = irange(0, 5);
        for (int i = 0; i < n; i++) l[i] = strs[rnd() % 6];
        l[n] = 0;
        w[0] = U(chat_text()); w[1] = U(l);
        return true;
    }
    case D_get_numeric: {
        char* t = chat_text();
        if (chance(50)) {
            char b[32];
            sprintf(b, " track %d", irange(-20, 99999));
            if (chance(30)) sprintf(b, " track %u%u", rnd() % 100000, rnd() % 100000);   // up to 10 digits
            strcat(t, b);
        }
        w[0] = U(t); w[1] = chance(50) ? 0x004fb6bc : 0x004fb6e4; w[2] = U(sv(4)); w[3] = (uint32_t)irange(0, 1);
        w[4] = (uint32_t)irange(0, 8);
        return true;
    }
    case D_contains: w[0] = U(chat_text()); w[1] = chance(50) ? 0x004fb744 : 0x004fb760; return true;
    case 0x004a36a0: {                                           // MultiDedicatedServer
        w[0] = chance(30) ? 0 : (rnd() & 0xffff) | (rnd() & 0xffff0000);
        add_line(chance(70) ? "Viper Test" : "", chance(50));
        add_line(chance(50) ? "" : "secret", chance(50));
        add_line(chance(50) ? "y" : "No", chance(50));
        s->quit_after = irange(1, 3);
        s->read_fail = 0;
        s->die_after = irange(0, 3);
        r->owner = s->task_cur;
        return true;
    }
    case D_ded_begin: case D_ded_end: return true;
    case D_ded_log_hook: { char* t = (char*)sv(64); t[irange(0, 63)] = 0; w[0] = U(t); return true; }
    case D_idle: add_random_lines(irange(0, 6)); if (chance(10)) *(uint8_t*)(uintptr_t)G_QUIT = 1; return true;
    case D_handle_input: {
        char* t = (char*)sv(64);
        strcpy(t, k_cmds[rnd() % (sizeof k_cmds / sizeof *k_cmds)]);
        w[0] = U(t);
        return true;
    }
    case D_prompt_user: {
        add_random_lines(1);
        w[0] = 0x004fb7cc; w[1] = U(sv(0x30)); w[2] = (uint32_t)irange(1, 0x20);
        return true;
    }
    default: return false;
    }
}

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
    char* sl = strstr(exe, "\\test\\world_net_server.cpp");
    if (sl) strcpy(sl, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    patch_jmp(0x004cf730, (void*)&stub_crt_fatal);         // _amsg_exit
    patch_jmp(0x004d45b0, (void*)&stub_crt_fatal);         // _NMSG_WRITE
    patch_jmp(0x004d4570, (void*)&stub_crt_fatal);         // _FF_MSGBANNER
    patch_jmp(0x004d5c10, (void*)&stub_crt_fatal);         // _fptrap
    patch_jmp(F_MemAlloc, (void*)&stub_MemAlloc);
    patch_jmp(F_op_delete, (void*)&stub_delete);
    patch_jmp(F_LogReport, (void*)&stub_LogReport);
    patch_jmp(F_LogPanic, (void*)&stub_LogPanic);
    patch_jmp(F_VERBOSE, (void*)&stub_VERBOSE);
    patch_jmp(S_ASSERT_MSG, (void*)&stub_ASSERT);
    patch_jmp(0x00413b40, (void*)&stub_PTimeNow);
    patch_jmp(F_TaskGetID, (void*)&stub_TaskGetID);
    patch_jmp(F_TaskGetName, (void*)&stub_TaskGetName);
    patch_jmp(F_TaskSleep, (void*)&stub_TaskSleep);
    patch_jmp(F_TaskCreate, (void*)&stub_TaskCreate);
    patch_jmp(F_TaskDestroy, (void*)&stub_TaskDestroy);
    patch_jmp(F_TaskShouldIDie, (void*)&stub_TaskShouldIDie);
    patch_jmp(F_sprintf, (void*)&stub_sprintf);
    patch_jmp(F_stricmp, (void*)&stub_stricmp);
    patch_jmp(F_strnicmp, (void*)&stub_strnicmp);
    patch_jmp(F_GetTrackName, (void*)&stub_GetTrackName);
    patch_jmp(F_GetLapCountFromType, (void*)&stub_GetLapCount);
    patch_jmp(F_CarFileMakeDefaultSetup, (void*)&stub_CarSetup);
    patch_jmp(F_FileOpen, (void*)&stub_FileOpen);
    patch_jmp(F_FileReadExact, (void*)&stub_FileReadExact);
    patch_jmp(F_FileClose, (void*)&stub_FileClose);
    patch_jmp(F_LogInstallHook, (void*)&stub_LogInstallHook);
    patch_jmp(F_LogUninstallHook, (void*)&stub_LogUninstallHook);
    patch_jmp(F_VersionGetBuildString, (void*)&stub_VersionString);
    patch_jmp(F_UsefulBegin, (void*)&stub_UsefulBegin);
    patch_jmp(F_UsefulEnd, (void*)&stub_UsefulEnd);
    patch_jmp(F_ResourceSetMustLoad, (void*)&stub_ResMustLoad);
    patch_jmp(F_ResourceSetUnload, (void*)&stub_ResUnload);
    patch_jmp(F_RaceBegin, (void*)&stub_RaceBegin);
    patch_jmp(F_RaceEnd, (void*)&stub_RaceEnd);
    patch_jmp(F_SocketCreateUDP, (void*)&stub_SocketCreateUDP);
    patch_jmp(F_CreateSessionMgr, (void*)&stub_CreateSessionMgr);
    patch_jmp(F_SM_Ok, (void*)&sm_Ok);
    patch_jmp(F_SM_dtor, (void*)&sm_dtor);
    patch_jmp(F_SM_CanDestroySafely, (void*)&sm_CanDestroy);
    patch_jmp(F_SM_Tick, (void*)&sm_Tick);
    patch_jmp(F_SM_Shutdown, (void*)&sm_Shutdown);
    patch_jmp(F_SM_OfferUnboundService, (void*)&sm_Offer);
    patch_jmp(F_SM_RefuseConnections, (void*)&sm_Refuse);
    patch_jmp(F_SM_AcceptConnections, (void*)&sm_Accept);
    patch_jmp(F_SM_WithdrawService, (void*)&sm_Withdraw);
    patch_jmp(F_SM_Disconnect, (void*)&sm_Disconnect);
    patch_jmp(F_SM_DisconnectService, (void*)&sm_DisconnectService);
    patch_jmp(F_SM_Send, (void*)&sm_Send);
    patch_jmp(F_SM_SendReliable, (void*)&sm_SendReliable);
    patch_jmp(F_SM_UnboundRecv, (void*)&sm_UnboundRecv);
    patch_jmp(F_SM_GetStatus, (void*)&sm_GetStatus);
    patch_jmp(F_SM_GetNextStatus, (void*)&sm_GetNextStatus);
    patch_jmp(F_RDP_GetEstRTLatency, (void*)&rdp_GetEstRTLatency);
    set_iat(I_AllocConsole, (void*)&k_AllocConsole);
    set_iat(I_GetStdHandle, (void*)&k_GetStdHandle);
    set_iat(I_SetConsoleMode, (void*)&k_SetConsoleMode);
    set_iat(I_SetConsoleTitleA, (void*)&k_SetConsoleTitleA);
    set_iat(I_WriteConsoleA, (void*)&k_WriteConsoleA);
    set_iat(I_ReadConsoleA, (void*)&k_ReadConsoleA);
    set_iat(I_FreeConsole, (void*)&k_FreeConsole);
    set_iat(I_Sleep, (void*)&k_Sleep);
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_pristine = mem_alloc(); g_snap = mem_alloc(); g_after = mem_alloc();
    HS()->brk = A_HEAP;
    // ded.obj's and server.obj's $E initialisers, once (the originals)
    g_logging = false;
    int ne = 0;
    for (uint32_t a = 0x004a2f30; a <= 0x004a3160; a += 0x10, ne++) call_orig(a, 0, {});
    for (uint32_t a = 0x004aa6a0; a <= 0x004aa8e0; a += 0x10, ne++) call_orig(a, 0, {});
    g_logging = true;
    printf("ran ded.obj's and server.obj's %d $E initialisers (class mask %02x, protocol table %02x %02x %02x...)\n", ne,
           hg8(G_SEND_CLASS_MASK), hg8(G_PROTOCOL), hg8(G_PROTOCOL + 1), hg8(G_PROTOCOL + 2));
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
    for (int fi = 0; fi < g_nfns; fi++) {
        const Ent& f = g_fns[fi];
        if (only[0] && !strstr(f.name, only)) continue;
        nfn++;
        const int n = rounds * 40;
        bool fn_bad = false;
        int fn_checks = 0, fn_faults = 0;
        uint32_t shapes[64];
        int nshapes = 0;
        for (int rd = 0; rd < n && !fn_bad; rd++) {
            mem_load(g_pristine);
            memset(g_arena, 0, ARENA_BYTES);
            uint32_t words[18];
            g_logging = false;
            build_world();
            const bool ok = args_for(f, words);
            g_logging = true;
            if (!ok) { skipped++; continue; }
            fp.n = 0; fp.replay_only = 0; fp.pure = false;
            f.fp(fp, words);
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
            bool same = ro.fault == rn.fault && ro.code == rn.code && ro.ret == rn.ret && ro.pops == rn.pops &&
                        !memcmp(ro.regs, rn.regs, sizeof ro.regs) && ro.top == rn.top;
            const uint32_t where = mem_diff(g_after);
            same &= where == 0;
            same &= g_log.n == g_log_orig.n && !memcmp(g_log.w, g_log_orig.w, 4 * (g_log.n < LOG_MAX ? g_log.n : LOG_MAX));
            if (!same) {
                printf("  MISMATCH %08x %s (round %d)\n", f.v10, f.name, rd);
                printf("    fault %d/%d (%08x at %08x / %08x at %08x), return %08x / %08x, popped %u / %u, x87 top %u / %u, "
                       "ebx esi edi ebp %08x %08x %08x %08x / %08x %08x %08x %08x\n",
                       ro.fault, rn.fault, ro.code, ro.eip, rn.code, rn.eip, ro.ret, rn.ret, ro.pops, rn.pops, ro.top, rn.top,
                       ro.regs[0], ro.regs[1], ro.regs[2], ro.regs[3], rn.regs[0], rn.regs[1], rn.regs[2], rn.regs[3]);
                if (where) {
                    const uint32_t a = U(g_arena);
                    if (where >= a && where < a + ARENA_BYTES) {
                        printf("    memory first differs at arena+%05x: %02x / %02x", where - a, g_after.arena[where - a], g_arena[where - a]);
                        const uint32_t rs = U(W.rs);
                        if (where >= rs && where < rs + 0x1198) printf(" (RaceServer +%04x)", where - rs);
                        printf("\n");
                    } else printf("    memory first differs at %08x\n", where);
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
        if (differ >= 40) { printf("stopping after 40 differences\n"); break; }
    }
    mem_load(g_pristine);
    printf("%d functions, %d listed twice; %lld checks (%d skipped: no world for it), %lld logged words compared, %lld checks "
           "where the original faulted (both alike unless counted below): %d differ, %d footprint violations; %d functions bad\n",
           nfn, dup, checks, skipped, log_words, faulted, differ, fp_bad, bad_fns);
    return differ || fp_bad || dup ? 1 : 0;
}
